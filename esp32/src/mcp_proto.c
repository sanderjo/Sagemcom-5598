#include "mcp_proto.h"
#include <stdio.h>
#include <string.h>

#define SERVER_NAME "sagemcom5598"
#define SERVER_VERSION "0.1.0-esp32"

// Newest first: what we answer when the client asks for something else.
static const char *const PROTOCOL_VERSIONS[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

static const char INSTRUCTIONS[] =
    "Tools for a Sagemcom F@st 5598 home gateway (Delta Fiber, NL) and its FAST381 mesh extenders,\n"
    "served from an ESP32-S3 on the LAN. For setup questions use `router_overview`, `network_topology`,\n"
    "`list_devices`, `list_extenders`, `wifi_details`, `wan_details`, `ethernet_ports`,\n"
    "`firewall_details`, `dhcp_details`. (The event log, history and `diagnose` are not on the ESP32 yet.)\n"
    "\n"
    "Notes: the router allows one admin session at a time, so each tool call logs in and out (a user\n"
    "logged into the web GUI at the same time may be logged out). Signal strengths are in dBm: better\n"
    "than -65 good, -65..-75 fair, below -75 weak. Nodes and clients that have a friendly name in\n"
    "nicknames.txt carry it as `nickname` (or `parent_nickname`, `connected_via_nickname`) next to the\n"
    "technical name; use it when talking to the user.";

static cJSON *response(const cJSON *id, cJSON *result)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(r, "result", result);
    return r;
}

static cJSON *error_response(const cJSON *id, int code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

cJSON *mcp_error(int code, const char *message)
{
    return error_response(NULL, code, message);
}

static cJSON *initialize(const cJSON *params)
{
    const char *asked = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "protocolVersion"));
    const char *version = PROTOCOL_VERSIONS[0];
    for (size_t i = 0; asked && i < sizeof(PROTOCOL_VERSIONS) / sizeof(PROTOCOL_VERSIONS[0]); i++)
        if (strcmp(asked, PROTOCOL_VERSIONS[i]) == 0) version = PROTOCOL_VERSIONS[i];

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddFalseToObject(cJSON_AddObjectToObject(caps, "tools"), "listChanged");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", SERVER_NAME);
    cJSON_AddStringToObject(info, "version", SERVER_VERSION);
    cJSON_AddStringToObject(result, "instructions", INSTRUCTIONS);
    return result;
}

static cJSON *text_result(const char *text, int is_error)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(result, "content"), item);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

static cJSON *call_tool(const mcp_server_t *server, const cJSON *id, const cJSON *params)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "name"));
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!tools_exists(name)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Unknown tool: %.60s", name ? name : "(none)");
        return error_response(id, MCP_INVALID_PARAMS, msg);
    }
    if (args && !cJSON_IsObject(args)) return error_response(id, MCP_INVALID_PARAMS, "arguments must be an object");

    char err[160];
    if (server->before_tool) server->before_tool(server->fetch_ctx, name);
    cJSON *out = tools_run(name, args, server->fetch, server->fetch_ctx, server->nicknames, server->nickname_count,
                           err, sizeof(err));
    if (server->after_tool) server->after_tool(server->fetch_ctx, name, out != NULL);
    if (!out) {
        char msg[220];
        snprintf(msg, sizeof(msg), "Error executing tool %s: %s", name, err[0] ? err : "unknown error");
        return response(id, text_result(msg, 1));
    }
    char *text = cJSON_Print(out);
    cJSON_Delete(out);
    if (!text) return error_response(id, -32603, "out of memory");
    cJSON *result = text_result(text, 0);
    cJSON_free(text);
    return response(id, result);
}

static cJSON *handle_one(const mcp_server_t *server, const cJSON *msg)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    if (!cJSON_IsObject(msg)) return error_response(NULL, MCP_INVALID_REQUEST, "Invalid Request");
    if (!method) {
        // a response to something we sent (we never send requests), or junk
        return id ? NULL : error_response(NULL, MCP_INVALID_REQUEST, "Invalid Request");
    }
    if (!id) return NULL;  // notification (notifications/initialized, cancelled, ...): nothing to answer
    if (!cJSON_IsString(id) && !cJSON_IsNumber(id)) return error_response(NULL, MCP_INVALID_REQUEST, "Invalid id");

    if (strcmp(method, "initialize") == 0) return response(id, initialize(params));
    if (strcmp(method, "ping") == 0) return response(id, cJSON_CreateObject());
    if (strcmp(method, "tools/list") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", tools_definitions());
        return response(id, result);
    }
    if (strcmp(method, "tools/call") == 0) return call_tool(server, id, params);
    // includes server/discover (2026-07-28): the error makes such clients fall back to initialize
    return error_response(id, MCP_METHOD_NOT_FOUND, "Method not found");
}

cJSON *mcp_handle(const mcp_server_t *server, const cJSON *message)
{
    if (!cJSON_IsArray(message)) return handle_one(server, message);
    if (!message->child) return error_response(NULL, MCP_INVALID_REQUEST, "Invalid Request");
    cJSON *batch = cJSON_CreateArray();
    const cJSON *msg;
    cJSON_ArrayForEach(msg, message) {
        cJSON *r = handle_one(server, msg);
        if (r) cJSON_AddItemToArray(batch, r);
    }
    if (!batch->child) {
        cJSON_Delete(batch);
        return NULL;
    }
    return batch;
}
