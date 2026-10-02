#include "tools.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef cJSON *(*tool_fn)(const cJSON *args, tools_fetch_fn fetch, void *ctx, char *err, size_t err_size);

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;  // JSON
    tool_fn run;
} tool_t;

// --- tools -------------------------------------------------------------

static cJSON *network_topology(const cJSON *args, tools_fetch_fn fetch, void *ctx, char *err, size_t err_size)
{
    (void)args;
    cJSON *mesh = fetch(ctx, "/api/v4/easymesh/meshdevices", err, err_size);
    if (!mesh) return NULL;
    cJSON *out = mesh_topology(mesh);
    cJSON_Delete(mesh);
    if (!out) snprintf(err, err_size, "no gateway in the router's mesh data");
    return out;
}

static int truthy(const cJSON *v)
{
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != '\0';
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsArray(v) || cJSON_IsObject(v)) return v->child != NULL;
    return 1;
}

static int same_mac(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static cJSON *list_devices(const cJSON *args, tools_fetch_fn fetch, void *ctx, char *err, size_t err_size)
{
    cJSON *mesh = fetch(ctx, "/api/v4/easymesh/meshdevices", err, err_size);
    if (!mesh) return NULL;
    cJSON *devices = mesh_devices(mesh);
    cJSON_Delete(mesh);
    if (!devices) {
        snprintf(err, err_size, "no mesh in the router's data");
        return NULL;
    }

    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "include_inactive"))) {
        cJSON *reply = fetch(ctx, "/api/v1/hosts", err, err_size);
        cJSON *hosts = reply ? mesh_hosts(reply) : NULL;
        cJSON_Delete(reply);
        if (!hosts) {
            if (reply) snprintf(err, err_size, "no host list in the router's data");
            cJSON_Delete(devices);
            return NULL;
        }
        int connected_count = cJSON_GetArraySize(devices);  // only the connected ones count as "connected"
        cJSON *h;
        cJSON_ArrayForEach(h, hosts) {
            if (truthy(cJSON_GetObjectItemCaseSensitive(h, "active"))) continue;
            const char *mac = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(h, "mac"));
            int known = 0;
            for (int i = 0; mac && i < connected_count && !known; i++) {
                const char *dmac = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(devices, i), "mac"));
                known = dmac && same_mac(mac, dmac);
            }
            if (known) continue;
            cJSON *offline = cJSON_Duplicate(h, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(offline, "connection");  // {**h, "connection": ...}
            cJSON_AddStringToObject(offline, "connection", "offline");
            cJSON_AddItemToArray(devices, offline);
        }
        cJSON_Delete(hosts);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "count", cJSON_GetArraySize(devices));
    cJSON_AddItemToObject(out, "devices", devices);
    return out;
}

static cJSON *list_extenders(const cJSON *args, tools_fetch_fn fetch, void *ctx, char *err, size_t err_size)
{
    (void)args;
    cJSON *mesh = fetch(ctx, "/api/v4/easymesh/meshdevices", err, err_size);
    if (!mesh) return NULL;
    cJSON *extenders = mesh_extenders(mesh);
    cJSON_Delete(mesh);
    if (!extenders) {
        snprintf(err, err_size, "no mesh in the router's data");
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "extenders", extenders);
    return out;
}

// descriptions and schemas exactly as the Python server reports them
static const tool_t TOOLS[] = {
    {"network_topology",
     "The mesh as a tree: gateway at the root, extenders nested under the\n"
     "node they backhaul through, each node with its directly connected\n"
     "clients (band, signal strength, link speed).",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"network_topologyArguments\"}",
     network_topology},
    {"list_devices",
     "Connected clients (wired and wireless) with IP, MAC, band, signal and\n"
     "the mesh node they're on. With `include_inactive`, also devices the\n"
     "router knows but that are currently offline (with last-seen time).",
     "{\"type\":\"object\",\"properties\":{\"include_inactive\":{\"default\":false,\"title\":\"Include Inactive\","
     "\"type\":\"boolean\"}},\"title\":\"list_devicesArguments\"}",
     list_devices},
    {"list_extenders",
     "Mesh extenders: model, firmware, uptime, parent node, and backhaul\n"
     "(Ethernet speed, or per-band wifi signal/quality/channel).",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"list_extendersArguments\"}",
     list_extenders},
};
#define TOOL_COUNT (sizeof(TOOLS) / sizeof(TOOLS[0]))

static const tool_t *find(const char *name)
{
    for (size_t i = 0; name && i < TOOL_COUNT; i++)
        if (strcmp(TOOLS[i].name, name) == 0) return &TOOLS[i];
    return NULL;
}

cJSON *tools_definitions(void)
{
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].description);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(TOOLS[i].input_schema));
        cJSON *ann = cJSON_AddObjectToObject(t, "annotations");
        cJSON_AddTrueToObject(ann, "readOnlyHint");
        cJSON_AddTrueToObject(ann, "idempotentHint");
        cJSON_AddFalseToObject(ann, "openWorldHint");
        cJSON_AddItemToArray(list, t);
    }
    return list;
}

int tools_exists(const char *name)
{
    return find(name) != NULL;
}

cJSON *tools_run(const char *name, const cJSON *args, tools_fetch_fn fetch, void *ctx,
                 const nickname_t *nicknames, size_t nickname_count, char *err, size_t err_size)
{
    const tool_t *tool = find(name);
    if (!tool) {
        snprintf(err, err_size, "Unknown tool: %s", name ? name : "(none)");
        return NULL;
    }
    err[0] = '\0';
    cJSON *out = tool->run(args, fetch, ctx, err, err_size);
    if (out) mesh_add_nicknames(out, nicknames, nickname_count);
    return out;
}
