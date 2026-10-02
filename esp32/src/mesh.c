#include "mesh.h"
#include "pyjson.h"
#include <ctype.h>
#include <string.h>

#define MAX_DEPTH 8  // guard against a backhaul loop when nesting extenders

static const cJSON *mesh_list(const cJSON *reply)
{
    return pj_get(cJSON_GetArrayItem(reply, 0), "meshDevices");
}

cJSON *mesh_extenders(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev, *other;
    if (!devices) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(dev, devices) {
        if (!pj_is_str(pj_get(dev, "type"), "extender")) continue;
        const cJSON *backhaul = pj_or(pj_get(dev, "backhaul"), NULL);
        cJSON *ext = cJSON_CreateObject();
        cJSON_AddItemToObject(ext, "hostname", pj_dup(pj_or(pj_get(dev, "hostname"), pj_get(dev, "deviceId"))));
        cJSON_AddItemToObject(ext, "device_id", pj_dup(pj_get(dev, "deviceId")));
        cJSON_AddItemToObject(ext, "model", pj_dup(pj_get(dev, "model")));
        cJSON_AddItemToObject(ext, "serial_number", pj_dup(pj_get(dev, "serialNumber")));
        cJSON_AddItemToObject(ext, "firmware", pj_dup(pj_get(dev, "softwareVersion")));
        cJSON_AddItemToObject(ext, "ipv4", pj_dup(pj_get(dev, "ipv4")));
        cJSON_AddItemToObject(ext, "uptime", pj_dup(pj_get(dev, "upTime")));

        // hostname_by_device_id: the last mesh device with that deviceId wins, like a dict
        const cJSON *parent = NULL;
        const cJSON *root_id = pj_get(backhaul, "rootDeviceId");
        cJSON_ArrayForEach(other, devices) {
            if (pj_same_key(pj_get(other, "deviceId"), root_id)) parent = pj_or(pj_get(other, "hostname"), pj_get(other, "deviceId"));
        }
        cJSON_AddItemToObject(ext, "parent", pj_dup(parent));
        cJSON_AddItemToObject(ext, "backhaul", backhaul ? cJSON_Duplicate(backhaul, 1) : cJSON_CreateObject());

        cJSON *signal = cJSON_CreateObject();
        const cJSON *link;
        cJSON_ArrayForEach(link, pj_get(backhaul, "wifiLinks")) {
            const cJSON *band = pj_get(link, "band");
            if (!cJSON_IsString(band)) continue;  // JSON keys must be strings
            const cJSON *strength = pj_get(link, "signalStrength");
            cJSON *value = pj_dup(strength ? strength : pj_get(link, "rssi0"));
            pj_set(signal, band->valuestring, value);
        }
        cJSON_AddItemToObject(ext, "signal_strength_dbm", signal);
        cJSON_AddItemToArray(out, ext);
    }
    return out;
}

static cJSON *client_name(const cJSON *c)
{
    return pj_dup(pj_or(pj_or(pj_get(c, "hostName"), pj_get(c, "friendlyname")), pj_get(c, "macAddress")));
}

cJSON *mesh_devices(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev;
    if (!devices) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(dev, devices) {
        const cJSON *radio, *ssid, *station, *port, *neighbour;
        cJSON_ArrayForEach(radio, pj_get(dev, "wifiRadios")) {
            cJSON_ArrayForEach(ssid, pj_get(radio, "ssids")) {
                cJSON_ArrayForEach(station, pj_get(ssid, "stations")) {
                    cJSON *d = cJSON_CreateObject();
                    cJSON_AddItemToObject(d, "name", client_name(station));
                    cJSON_AddItemToObject(d, "ip", pj_dup(pj_get(station, "ipv4Address")));
                    cJSON_AddItemToObject(d, "mac", pj_dup(pj_get(station, "macAddress")));
                    cJSON_AddStringToObject(d, "connection", "wireless");
                    cJSON_AddItemToObject(d, "band", pj_dup(pj_get(radio, "band")));
                    cJSON_AddItemToObject(d, "signal_strength", pj_dup(pj_get(station, "signalStrength")));
                    cJSON_AddItemToObject(d, "ssid", pj_dup(pj_get(ssid, "ssid")));
                    cJSON_AddItemToObject(d, "link_quality", pj_dup(pj_get(station, "linkQuality")));
                    cJSON_AddItemToObject(d, "connected_via", pj_dup(pj_get(dev, "hostname")));
                    cJSON_AddItemToArray(out, d);
                }
            }
        }
        cJSON_ArrayForEach(port, pj_get(dev, "ethernetPorts")) {
            cJSON_ArrayForEach(neighbour, pj_get(port, "neighbours")) {
                cJSON *d = cJSON_CreateObject();
                cJSON_AddItemToObject(d, "name", client_name(neighbour));
                cJSON_AddItemToObject(d, "ip", pj_dup(pj_get(neighbour, "ipv4Address")));
                cJSON_AddItemToObject(d, "mac", pj_dup(pj_get(neighbour, "macAddress")));
                cJSON_AddStringToObject(d, "connection", "wired");
                cJSON_AddNullToObject(d, "band");
                cJSON_AddNullToObject(d, "signal_strength");
                cJSON_AddNullToObject(d, "ssid");
                cJSON_AddNullToObject(d, "link_quality");
                cJSON_AddItemToObject(d, "connected_via", pj_dup(pj_get(dev, "hostname")));
                cJSON_AddItemToObject(d, "link_speed_mbps", pj_dup(pj_get(port, "speed")));
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
    cJSON_AddItemToObject(node, "hostname", pj_dup(hostname));
    cJSON_AddItemToObject(node, "ipv4", pj_dup(ipv4));
    cJSON_AddItemToObject(node, "backhaul_type", pj_dup(backhaul_type));
    cJSON_AddItemToObject(node, "signal_strength_dbm", pj_dup(signal));

    // clients on this node, wired first (stable, like sort(key=connection != "wired"))
    cJSON *node_clients = cJSON_AddArrayToObject(node, "clients");
    const cJSON *c;
    for (int wired = 1; wired >= 0; wired--) {
        cJSON_ArrayForEach(c, clients) {
            if (pj_same_key(pj_get(c, "connected_via"), hostname) && pj_is_str(pj_get(c, "connection"), "wired") == wired)
                cJSON_AddItemToArray(node_clients, cJSON_Duplicate(c, 1));
        }
    }

    cJSON *children = cJSON_AddArrayToObject(node, "extenders");
    const cJSON *ext;
    if (depth >= MAX_DEPTH) return node;
    cJSON_ArrayForEach(ext, extenders) {
        if (!pj_same_key(pj_get(ext, "parent"), hostname)) continue;
        cJSON_AddItemToArray(children, build_node(pj_get(ext, "hostname"), pj_get(ext, "ipv4"), pj_get(ext, "signal_strength_dbm"),
                                                  pj_get(pj_get(ext, "backhaul"), "linkType"), extenders, clients, depth + 1));
    }
    return node;
}

cJSON *mesh_topology(const cJSON *reply)
{
    const cJSON *devices = mesh_list(reply), *dev, *gateway = NULL;
    cJSON_ArrayForEach(dev, devices) {
        if (pj_is_str(pj_get(dev, "type"), "gateway")) { gateway = dev; break; }
    }
    if (!gateway) return NULL;
    cJSON *extenders = mesh_extenders(reply), *clients = mesh_devices(reply);
    cJSON *tree = build_node(pj_get(gateway, "hostname"), pj_get(gateway, "ipv4"), NULL, NULL, extenders, clients, 0);
    cJSON_Delete(extenders);
    cJSON_Delete(clients);
    return tree;
}

cJSON *mesh_hosts(const cJSON *reply)
{
    const cJSON *list = pj_get(pj_get(cJSON_GetArrayItem(reply, 0), "hosts"), "list"), *host;
    if (!cJSON_IsArray(list)) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(host, list) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddItemToObject(h, "name", pj_dup(pj_or(pj_or(pj_get(host, "friendlyHostname"), pj_get(host, "hostname")), pj_get(host, "macaddress"))));
        cJSON_AddItemToObject(h, "mac", pj_dup(pj_get(host, "macaddress")));
        cJSON_AddItemToObject(h, "ip", pj_dup(pj_get(host, "ipaddress")));
        cJSON *ipv6 = cJSON_AddArrayToObject(h, "ipv6");
        const cJSON *addr;
        cJSON_ArrayForEach(addr, pj_get(host, "ip6address")) cJSON_AddItemToArray(ipv6, pj_dup(pj_get(addr, "ipaddress")));
        cJSON_AddItemToObject(h, "active", pj_dup(pj_get(host, "active")));
        cJSON_AddItemToObject(h, "link", pj_dup(pj_get(host, "link")));
        cJSON_AddItemToObject(h, "address_type", pj_dup(pj_get(host, "type")));
        cJSON_AddItemToObject(h, "lease_remaining", pj_dup(pj_get(host, "lease")));
        cJSON_AddItemToObject(h, "last_seen", pj_dup(pj_get(host, "lastseen")));
        cJSON_AddItemToObject(h, "device_type", pj_dup(pj_get(host, "devicetype")));
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
