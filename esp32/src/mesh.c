#include "mesh.h"
#include <ctype.h>
#include <string.h>

#define MAX_DEPTH 8  // guard against a backhaul loop when nesting extenders

// Python truthiness of a JSON value
static int truthy(const cJSON *v)
{
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != '\0';
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsArray(v) || cJSON_IsObject(v)) return v->child != NULL;
    return 1;
}

// `a or b` (missing counts as None)
static const cJSON *py_or(const cJSON *a, const cJSON *b)
{
    return truthy(a) ? a : b;
}

static const cJSON *get(const cJSON *obj, const char *key)
{
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

// copy of a value, or null when missing (Python's dict.get -> None)
static cJSON *dup(const cJSON *v)
{
    return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
}

// equality used for dict keys: both None, or equal strings/numbers
static int same_key(const cJSON *a, const cJSON *b)
{
    int a_none = !a || cJSON_IsNull(a), b_none = !b || cJSON_IsNull(b);
    if (a_none || b_none) return a_none && b_none;
    if (cJSON_IsString(a) && cJSON_IsString(b)) return strcmp(a->valuestring, b->valuestring) == 0;
    if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) return a->valuedouble == b->valuedouble;
    return 0;
}

static int is_str(const cJSON *v, const char *s)
{
    return cJSON_IsString(v) && strcmp(v->valuestring, s) == 0;
}

static const cJSON *mesh_list(const cJSON *reply)
{
    return get(cJSON_GetArrayItem(reply, 0), "meshDevices");
}

cJSON *mesh_extenders(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev, *other;
    if (!devices) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(dev, devices) {
        if (!is_str(get(dev, "type"), "extender")) continue;
        const cJSON *backhaul = py_or(get(dev, "backhaul"), NULL);
        cJSON *ext = cJSON_CreateObject();
        cJSON_AddItemToObject(ext, "hostname", dup(py_or(get(dev, "hostname"), get(dev, "deviceId"))));
        cJSON_AddItemToObject(ext, "device_id", dup(get(dev, "deviceId")));
        cJSON_AddItemToObject(ext, "model", dup(get(dev, "model")));
        cJSON_AddItemToObject(ext, "serial_number", dup(get(dev, "serialNumber")));
        cJSON_AddItemToObject(ext, "firmware", dup(get(dev, "softwareVersion")));
        cJSON_AddItemToObject(ext, "ipv4", dup(get(dev, "ipv4")));
        cJSON_AddItemToObject(ext, "uptime", dup(get(dev, "upTime")));

        // hostname_by_device_id: the last mesh device with that deviceId wins, like a dict
        const cJSON *parent = NULL;
        const cJSON *root_id = get(backhaul, "rootDeviceId");
        cJSON_ArrayForEach(other, devices) {
            if (same_key(get(other, "deviceId"), root_id)) parent = py_or(get(other, "hostname"), get(other, "deviceId"));
        }
        cJSON_AddItemToObject(ext, "parent", dup(parent));
        cJSON_AddItemToObject(ext, "backhaul", backhaul ? cJSON_Duplicate(backhaul, 1) : cJSON_CreateObject());

        cJSON *signal = cJSON_CreateObject();
        const cJSON *link;
        cJSON_ArrayForEach(link, get(backhaul, "wifiLinks")) {
            const cJSON *band = get(link, "band");
            if (!cJSON_IsString(band)) continue;  // JSON keys must be strings
            const cJSON *strength = get(link, "signalStrength");
            cJSON *value = dup(strength ? strength : get(link, "rssi0"));
            if (cJSON_GetObjectItemCaseSensitive(signal, band->valuestring))
                cJSON_ReplaceItemInObjectCaseSensitive(signal, band->valuestring, value);
            else
                cJSON_AddItemToObject(signal, band->valuestring, value);
        }
        cJSON_AddItemToObject(ext, "signal_strength_dbm", signal);
        cJSON_AddItemToArray(out, ext);
    }
    return out;
}

static cJSON *client_name(const cJSON *c)
{
    return dup(py_or(py_or(get(c, "hostName"), get(c, "friendlyname")), get(c, "macAddress")));
}

cJSON *mesh_devices(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev;
    if (!devices) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(dev, devices) {
        const cJSON *radio, *ssid, *station, *port, *neighbour;
        cJSON_ArrayForEach(radio, get(dev, "wifiRadios")) {
            cJSON_ArrayForEach(ssid, get(radio, "ssids")) {
                cJSON_ArrayForEach(station, get(ssid, "stations")) {
                    cJSON *d = cJSON_CreateObject();
                    cJSON_AddItemToObject(d, "name", client_name(station));
                    cJSON_AddItemToObject(d, "ip", dup(get(station, "ipv4Address")));
                    cJSON_AddItemToObject(d, "mac", dup(get(station, "macAddress")));
                    cJSON_AddStringToObject(d, "connection", "wireless");
                    cJSON_AddItemToObject(d, "band", dup(get(radio, "band")));
                    cJSON_AddItemToObject(d, "signal_strength", dup(get(station, "signalStrength")));
                    cJSON_AddItemToObject(d, "ssid", dup(get(ssid, "ssid")));
                    cJSON_AddItemToObject(d, "link_quality", dup(get(station, "linkQuality")));
                    cJSON_AddItemToObject(d, "connected_via", dup(get(dev, "hostname")));
                    cJSON_AddItemToArray(out, d);
                }
            }
        }
        cJSON_ArrayForEach(port, get(dev, "ethernetPorts")) {
            cJSON_ArrayForEach(neighbour, get(port, "neighbours")) {
                cJSON *d = cJSON_CreateObject();
                cJSON_AddItemToObject(d, "name", client_name(neighbour));
                cJSON_AddItemToObject(d, "ip", dup(get(neighbour, "ipv4Address")));
                cJSON_AddItemToObject(d, "mac", dup(get(neighbour, "macAddress")));
                cJSON_AddStringToObject(d, "connection", "wired");
                cJSON_AddNullToObject(d, "band");
                cJSON_AddNullToObject(d, "signal_strength");
                cJSON_AddNullToObject(d, "ssid");
                cJSON_AddNullToObject(d, "link_quality");
                cJSON_AddItemToObject(d, "connected_via", dup(get(dev, "hostname")));
                cJSON_AddItemToObject(d, "link_speed_mbps", dup(get(port, "speed")));
                cJSON_AddItemToArray(out, d);
            }
        }
    }
    return out;
}

static cJSON *build_node(const cJSON *hostname, const cJSON *ipv4, const cJSON *signal, const cJSON *backhaul_type,
                         const cJSON *extenders, const cJSON *clients, int depth)
{
    cJSON *node = cJSON_CreateObject();
    cJSON_AddItemToObject(node, "hostname", dup(hostname));
    cJSON_AddItemToObject(node, "ipv4", dup(ipv4));
    cJSON_AddItemToObject(node, "backhaul_type", dup(backhaul_type));
    cJSON_AddItemToObject(node, "signal_strength_dbm", dup(signal));

    // clients on this node, wired first (stable, like sort(key=connection != "wired"))
    cJSON *node_clients = cJSON_AddArrayToObject(node, "clients");
    const cJSON *c;
    for (int wired = 1; wired >= 0; wired--) {
        cJSON_ArrayForEach(c, clients) {
            if (same_key(get(c, "connected_via"), hostname) && is_str(get(c, "connection"), "wired") == wired)
                cJSON_AddItemToArray(node_clients, cJSON_Duplicate(c, 1));
        }
    }

    cJSON *children = cJSON_AddArrayToObject(node, "extenders");
    const cJSON *ext;
    if (depth >= MAX_DEPTH) return node;
    cJSON_ArrayForEach(ext, extenders) {
        if (!same_key(get(ext, "parent"), hostname)) continue;
        cJSON_AddItemToArray(children, build_node(get(ext, "hostname"), get(ext, "ipv4"), get(ext, "signal_strength_dbm"),
                                                  get(get(ext, "backhaul"), "linkType"), extenders, clients, depth + 1));
    }
    return node;
}

cJSON *mesh_topology(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev, *gateway = NULL;
    cJSON_ArrayForEach(dev, devices) {
        if (is_str(get(dev, "type"), "gateway")) { gateway = dev; break; }
    }
    if (!gateway) return NULL;
    cJSON *extenders = mesh_extenders(reply), *clients = mesh_devices(reply);
    cJSON *tree = build_node(get(gateway, "hostname"), get(gateway, "ipv4"), NULL, NULL, extenders, clients, 0);
    cJSON_Delete(extenders);
    cJSON_Delete(clients);
    return tree;
}

cJSON *mesh_hosts(const cJSON *reply)
{
    const cJSON *list = get(get(cJSON_GetArrayItem(reply, 0), "hosts"), "list"), *host;
    if (!cJSON_IsArray(list)) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(host, list) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddItemToObject(h, "name", dup(py_or(py_or(get(host, "friendlyHostname"), get(host, "hostname")), get(host, "macaddress"))));
        cJSON_AddItemToObject(h, "mac", dup(get(host, "macaddress")));
        cJSON_AddItemToObject(h, "ip", dup(get(host, "ipaddress")));
        cJSON *ipv6 = cJSON_AddArrayToObject(h, "ipv6");
        const cJSON *addr;
        cJSON_ArrayForEach(addr, get(host, "ip6address")) cJSON_AddItemToArray(ipv6, dup(get(addr, "ipaddress")));
        cJSON_AddItemToObject(h, "active", dup(get(host, "active")));
        cJSON_AddItemToObject(h, "link", dup(get(host, "link")));
        cJSON_AddItemToObject(h, "address_type", dup(get(host, "type")));
        cJSON_AddItemToObject(h, "lease_remaining", dup(get(host, "lease")));
        cJSON_AddItemToObject(h, "last_seen", dup(get(host, "lastseen")));
        cJSON_AddItemToObject(h, "device_type", dup(get(host, "devicetype")));
        cJSON_AddItemToArray(out, h);
    }
    return out;
}

// Fields naming a mesh node or client, and the field their nickname goes in
static const char *const NAME_FIELDS[][2] = {
    {"hostname", "nickname"}, {"name", "nickname"}, {"mac", "nickname"}, {"device_id", "nickname"},
    {"parent", "parent_nickname"}, {"connected_via", "connected_via_nickname"}, {"via", "via_nickname"},
};

static const char *lookup(const char *name, const nickname_t *nicknames, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const char *a = name, *b = nicknames[i].name;
        while (*a && tolower((unsigned char)*a) == *b) a++, b++;
        if (!*a && !*b) return nicknames[i].nickname;
    }
    return NULL;
}

void mesh_add_nicknames(cJSON *data, const nickname_t *nicknames, size_t count)
{
    if (cJSON_IsObject(data)) {
        for (size_t f = 0; f < sizeof(NAME_FIELDS) / sizeof(NAME_FIELDS[0]); f++) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, NAME_FIELDS[f][0]);
            const char *nick = cJSON_IsString(value) ? lookup(value->valuestring, nicknames, count) : NULL;
            if (nick && !cJSON_GetObjectItemCaseSensitive(data, NAME_FIELDS[f][1]))
                cJSON_AddStringToObject(data, NAME_FIELDS[f][1], nick);
        }
    }
    if (cJSON_IsObject(data) || cJSON_IsArray(data)) {
        cJSON *child;
        cJSON_ArrayForEach(child, data) mesh_add_nicknames(child, nicknames, count);
    }
}
