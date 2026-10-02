#include "tools.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "pyjson.h"

typedef struct {
    tools_fetch_fn fetch;
    void *ctx;
    pj_err_t *err;
} router_t;

typedef cJSON *(*tool_fn)(const cJSON *args, router_t *r);

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;  // JSON
    tool_fn run;
} tool_t;

// GET a router path (NULL once anything failed, like an exception unwinding)
static cJSON *get_json(router_t *r, const char *path)
{
    if (r->err->failed) return NULL;
    char msg[160] = "";
    cJSON *json = r->fetch(r->ctx, path, msg, sizeof(msg));
    if (!json) pj_fail(r->err, "%s", msg[0] ? msg : "router request failed");
    return json;
}

// {k: int(v) for k, v in obj.items()}
static cJSON *int_map(const cJSON *obj, pj_err_t *err)
{
    if (!cJSON_IsObject(obj)) {
        pj_fail(err, "expected a dict of counters");
        return cJSON_CreateObject();
    }
    cJSON *out = cJSON_CreateObject();
    const cJSON *v;
    cJSON_ArrayForEach(v, obj) {
        cJSON *n = pj_int(v, err);
        pj_set(out, v->string, n ? n : cJSON_CreateNull());
    }
    return out;
}

static void add(cJSON *obj, const char *key, cJSON *value)
{
    cJSON_AddItemToObject(obj, key, value ? value : cJSON_CreateNull());
}

// --- ports of the Sagemcom5598 client methods ----------------------------

static cJSON *wan_ipv4(const cJSON *reply, pj_err_t *err)
{
    const cJSON *ipv4 = pj_req(pj_req(pj_item(reply, 0, err), "wan", err), "ipv4", err);
    cJSON *out = cJSON_CreateObject();
    add(out, "status", pj_dup(pj_req(ipv4, "status", err)));
    add(out, "addressing_type", pj_dup(pj_req(ipv4, "addressing_type", err)));
    add(out, "address", pj_dup(pj_req(ipv4, "address", err)));
    add(out, "subnet", pj_dup(pj_req(ipv4, "subnet", err)));
    add(out, "gateway", pj_dup(pj_req(ipv4, "gateway", err)));
    add(out, "uptime", pj_int(pj_req(ipv4, "uptime", err), err));
    add(out, "mac_address", pj_dup(pj_req(ipv4, "mac_address", err)));
    return out;
}

static cJSON *wan_status(const cJSON *reply, pj_err_t *err)
{
    const cJSON *s = pj_item(reply, 0, err);
    cJSON *out = cJSON_CreateObject();
    add(out, "status", pj_dup(pj_req(s, "status", err)));
    add(out, "last_change", pj_int(pj_req(s, "lastchange", err), err));
    return out;
}

static cJSON *wan_stats(const cJSON *reply, pj_err_t *err)
{
    const cJSON *stats = pj_req(pj_req(pj_req(pj_item(reply, 0, err), "wan", err), "ip", err), "stats", err);
    cJSON *out = cJSON_CreateObject();
    add(out, "rx", int_map(pj_req(stats, "rx", err), err));
    add(out, "tx", int_map(pj_req(stats, "tx", err), err));
    return out;
}

// str(v).isdigit() for a JSON value
static int str_isdigit(const cJSON *v)
{
    if (cJSON_IsString(v)) {
        const char *p = v->valuestring;
        if (!*p) return 0;
        for (; *p; p++)
            if (!isdigit((unsigned char)*p)) return 0;
        return 1;
    }
    // an int prints as digits (cJSON can't tell 1000 from 1000.0; the router sends ints)
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble == (double)(long long)v->valuedouble;
}

static cJSON *lan_ports(const cJSON *reply, pj_err_t *err)
{
    const cJSON *interfaces = pj_req(pj_req(pj_item(reply, 0, err), "lan", err), "interfaces", err), *intf;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(intf, interfaces) {
        cJSON *p = cJSON_CreateObject();
        add(p, "name", pj_dup(pj_get(intf, "name")));
        add(p, "alias", pj_dup(pj_get(intf, "alias")));
        add(p, "role", pj_dup(pj_get(intf, "role")));
        add(p, "enabled", pj_dup(pj_get(intf, "enable")));
        add(p, "status", pj_dup(pj_get(intf, "status")));
        const cJSON *rate = pj_get(intf, "curbitrate");
        add(p, "speed_mbps", str_isdigit(rate) ? pj_int(rate, err) : NULL);
        add(p, "duplex", pj_dup(pj_get(intf, "currentduplex")));
        const cJSON *rx = pj_get(intf, "rx"), *tx = pj_get(intf, "tx");
        add(p, "rx", rx ? int_map(rx, err) : cJSON_CreateObject());
        add(p, "tx", tx ? int_map(tx, err) : cJSON_CreateObject());
        cJSON_AddItemToArray(out, p);
    }
    return out;
}

static cJSON *device_info(const cJSON *reply, pj_err_t *err)
{
    const cJSON *info = pj_item(reply, 0, err);
    cJSON *out = cJSON_CreateObject();
    add(out, "uptime", pj_int(pj_req(info, "uptime", err), err));
    add(out, "serial_number", pj_dup(pj_req(info, "serial_number", err)));
    add(out, "firmware", pj_dup(pj_req(info, "firmware", err)));
    add(out, "wan_status", pj_dup(pj_req(info, "wan_status", err)));
    add(out, "wan_ipv4", pj_dup(pj_req(info, "wan_ipv4", err)));
    return out;
}

static cJSON *gateway_node(const cJSON *reply, pj_err_t *err)
{
    const cJSON *dev, *gateway = NULL;
    cJSON_ArrayForEach(dev, pj_get(pj_item(reply, 0, err), "meshDevices")) {
        if (pj_is_str(pj_get(dev, "type"), "gateway")) { gateway = dev; break; }
    }
    if (!gateway) pj_fail(err, "no gateway in the router's mesh data");
    cJSON *out = cJSON_CreateObject();
    add(out, "hostname", pj_dup(pj_get(gateway, "hostname")));
    add(out, "device_id", pj_dup(pj_get(gateway, "deviceId")));
    add(out, "model", pj_dup(pj_get(gateway, "model")));
    add(out, "serial_number", pj_dup(pj_get(gateway, "serialNumber")));
    add(out, "firmware", pj_dup(pj_get(gateway, "softwareVersion")));
    add(out, "ipv4", pj_dup(pj_get(gateway, "ipv4")));
    return out;
}

static cJSON *ntp(const cJSON *reply, pj_err_t *err)
{
    const cJSON *n = pj_req(reply, "ntp", err);
    cJSON *out = cJSON_CreateObject();
    add(out, "enabled", pj_dup(pj_get(n, "enable")));
    add(out, "status", pj_dup(pj_get(n, "status")));
    add(out, "now", pj_dup(pj_get(n, "now")));
    add(out, "time_zone", pj_dup(pj_get(n, "time_zone_name")));
    cJSON *servers = cJSON_AddArrayToObject(out, "servers");
    for (int i = 1; i <= 5; i++) {
        char key[24];
        snprintf(key, sizeof(key), "server%d", i);
        if (pj_truthy(pj_get(n, key))) cJSON_AddItemToArray(servers, pj_dup(pj_get(n, key)));
    }
    return out;
}

static cJSON *dhcp(const cJSON *reply, pj_err_t *err)
{
    const cJSON *info = pj_item(reply, 0, err), *d = pj_req(info, "dhcp", err);
    cJSON *out = cJSON_CreateObject();
    add(out, "enabled", pj_dup(pj_get(d, "enable")));
    add(out, "pool_start", pj_dup(pj_get(d, "minaddress")));
    add(out, "pool_end", pj_dup(pj_get(d, "maxaddress")));
    add(out, "lease_time", pj_dup(pj_get(d, "leasetime")));
    add(out, "router_ip", pj_dup(pj_get(d, "iprouter")));
    add(out, "subnet_mask", pj_dup(pj_get(d, "subnetmask")));
    const cJSON *list = pj_get(pj_get(info, "reservedpools"), "list");
    add(out, "reserved_pools", list ? pj_dup(list) : cJSON_CreateArray());
    return out;
}

static cJSON *wifi_config(const cJSON *home_reply, const cJSON *mesh_reply, const cJSON *steering_reply,
                          const cJSON *mlo_reply, pj_err_t *err)
{
    const cJSON *home = pj_item(home_reply, 0, err), *mesh = pj_item(mesh_reply, 0, err), *s, *dev, *radio;
    cJSON *out = cJSON_CreateObject();
    cJSON *ssids = cJSON_AddArrayToObject(out, "ssids");
    cJSON_ArrayForEach(s, pj_get(home, "ssids")) {
        cJSON *o = cJSON_CreateObject();
        add(o, "name", pj_dup(pj_get(s, "ssidName")));
        add(o, "type", pj_dup(pj_get(s, "type")));
        add(o, "radio", pj_dup(pj_get(s, "radio")));
        add(o, "status", pj_dup(pj_get(s, "ssidStatus")));
        add(o, "security", pj_dup(pj_get(s, "protocol")));
        add(o, "max_bitrate", pj_dup(pj_get(s, "maxbitrate")));
        cJSON_AddItemToArray(ssids, o);
    }
    cJSON *channels = cJSON_AddObjectToObject(out, "channels");
    cJSON_ArrayForEach(dev, pj_get(mesh, "meshDevices")) {
        char key[96];
        cJSON *bands = cJSON_CreateObject();
        cJSON_ArrayForEach(radio, pj_get(dev, "wifiRadios")) {
            pj_key_str(pj_get(radio, "band"), key, sizeof(key));
            pj_set(bands, key, pj_dup(pj_get(radio, "channel")));
        }
        pj_key_str(pj_get(dev, "hostname"), key, sizeof(key));
        pj_set(channels, key, bands);
    }
    add(out, "band_steering", pj_dup(pj_get(pj_item(steering_reply, 0, err), "BandSteeringEnable")));
    cJSON_AddBoolToObject(out, "mlo_enabled", pj_is_str(pj_get(pj_item(mlo_reply, 0, err), "mlo_state"), "1"));
    return out;
}

static cJSON *wifi_stats(router_t *r)
{
    static const char *const BANDS[][2] = {{"2.4", "24"}, {"5", "5"}, {"6", "6"}};
    cJSON *out = cJSON_CreateObject();
    for (int i = 0; i < 3; i++) {
        char path[48];
        snprintf(path, sizeof(path), "/api/v2/wireless/stats/%s", BANDS[i][1]);
        cJSON *reply = get_json(r, path);
        const cJSON *ssid = pj_req(pj_req(pj_item(reply, 0, r->err), "wireless", r->err), "ssid", r->err);
        cJSON *o = cJSON_CreateObject();
        add(o, "status", pj_dup(pj_get(ssid, "status")));
        add(o, "max_bitrate_mbps", pj_dup(pj_get(ssid, "maxbitrate")));
        add(o, "rx", pj_dup(pj_get(pj_get(ssid, "stats"), "rx")));
        add(o, "tx", pj_dup(pj_get(pj_get(ssid, "stats"), "tx")));
        cJSON_AddItemToObject(out, BANDS[i][0], o);
        cJSON_Delete(reply);
    }
    return out;
}

static cJSON *firewall_settings(const cJSON *fw_reply, const cJSON *chain_reply, pj_err_t *err)
{
    const cJSON *firewall = pj_req(pj_item(fw_reply, 0, err), "firewall", err);
    const cJSON *chain = pj_item(chain_reply, 0, err), *rule;
    cJSON *rules = cJSON_CreateArray();
    cJSON_ArrayForEach(rule, pj_get(chain, "rules")) {
        cJSON *o = cJSON_CreateObject();
        add(o, "id", pj_dup(pj_get(rule, "id")));
        add(o, "alias", pj_dup(pj_get(rule, "alias")));
        add(o, "description", pj_dup(pj_get(rule, "description")));
        add(o, "enabled", pj_dup(pj_get(rule, "enable")));
        add(o, "action", pj_dup(pj_get(rule, "action")));
        cJSON_AddStringToObject(o, "ip_version", pj_is_str(pj_get(rule, "ip_protocol"), "ipv6") ? "ipv6" : "ipv4");
        add(o, "protocol", pj_dup(pj_get(rule, "protocol")));
        add(o, "src_ip", pj_dup(pj_get(rule, "src_ip")));
        add(o, "src_ports", pj_dup(pj_get(rule, "src_ports")));
        add(o, "src_interface", pj_dup(pj_get(rule, "src_intf")));
        add(o, "dst_ip", pj_dup(pj_get(rule, "dst_ip")));
        add(o, "dst_ports", pj_dup(pj_get(rule, "dst_ports")));
        add(o, "dst_interface", pj_dup(pj_get(rule, "dst_intf")));
        cJSON_AddItemToArray(rules, o);
    }
    cJSON *out = cJSON_CreateObject();
    add(out, "level", pj_dup(pj_get(firewall, "level")));
    add(out, "port_scan_detection", pj_dup(pj_get(firewall, "port_scan_detection")));
    add(out, "block_fragmented_ip_packets", pj_dup(pj_get(firewall, "block_fragmented_ip_packets")));
    add(out, "custom_chain_enabled", pj_dup(pj_get(chain, "enable")));
    add(out, "default_policy", pj_dup(pj_get(chain, "default_policy")));
    add(out, "rules", rules);
    return out;
}

// --- tools (sagemcom5598_mcp.py) ---------------------------------------------
// Each fetches every router path it needs once; the Python client fetches
// meshdevices once per method, which reads the same data.

#define WITH(var, path) cJSON *var = get_json(r, path)

static cJSON *router_overview(const cJSON *args, router_t *r)
{
    (void)args;
    pj_err_t *err = r->err;
    WITH(open, "/api/v1/open");
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    WITH(wan, "/api/v1/wan/ipv4");
    WITH(ntp_reply, "/api/v1/ntp");
    cJSON *out = NULL;
    if (!err->failed) {
        cJSON *device = device_info(open, err), *gateway = gateway_node(mesh, err);
        cJSON *extenders = mesh_extenders(mesh), *devices = mesh_devices(mesh);
        out = cJSON_CreateObject();
        add(out, "model", pj_dup(pj_get(gateway, "model")));
        add(out, "hostname", pj_dup(pj_get(gateway, "hostname")));
        add(out, "serial_number", pj_dup(pj_get(device, "serial_number")));
        add(out, "firmware", pj_dup(pj_get(device, "firmware")));
        add(out, "uptime_seconds", pj_dup(pj_get(device, "uptime")));
        add(out, "lan_ip", pj_dup(pj_get(gateway, "ipv4")));
        add(out, "wan", wan_ipv4(wan, err));
        add(out, "ntp", ntp(ntp_reply, err));
        cJSON *ext_list = cJSON_AddArrayToObject(out, "extenders");
        static const char *const EXT_KEYS[] = {"hostname", "model", "firmware", "ipv4", "parent"};
        const cJSON *e, *d;
        cJSON_ArrayForEach(e, extenders) {
            cJSON *o = cJSON_CreateObject();
            for (size_t i = 0; i < sizeof(EXT_KEYS) / sizeof(EXT_KEYS[0]); i++)
                add(o, EXT_KEYS[i], pj_dup(pj_get(e, EXT_KEYS[i])));
            cJSON_AddItemToArray(ext_list, o);
        }
        int wired = 0, wireless = 0;
        cJSON_ArrayForEach(d, devices) {
            wired += pj_is_str(pj_get(d, "connection"), "wired");
            wireless += pj_is_str(pj_get(d, "connection"), "wireless");
        }
        cJSON *clients = cJSON_AddObjectToObject(out, "clients");
        cJSON_AddNumberToObject(clients, "wired", wired);
        cJSON_AddNumberToObject(clients, "wireless", wireless);
        if (!extenders || !devices) pj_fail(err, "no mesh in the router's data");
        cJSON_Delete(device);
        cJSON_Delete(gateway);
        cJSON_Delete(extenders);
        cJSON_Delete(devices);
    }
    cJSON_Delete(open);
    cJSON_Delete(mesh);
    cJSON_Delete(wan);
    cJSON_Delete(ntp_reply);
    return out;
}

static cJSON *network_topology(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    cJSON *out = mesh ? mesh_topology(mesh) : NULL;
    if (mesh && !out) pj_fail(r->err, "no gateway in the router's mesh data");
    cJSON_Delete(mesh);
    return out;
}

static int same_mac(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static cJSON *list_devices(const cJSON *args, router_t *r)
{
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    cJSON *devices = mesh ? mesh_devices(mesh) : NULL;
    cJSON_Delete(mesh);
    if (!devices) {
        pj_fail(r->err, "no mesh in the router's data");
        return NULL;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "include_inactive"))) {
        WITH(reply, "/api/v1/hosts");
        cJSON *hosts = reply ? mesh_hosts(reply) : NULL;
        cJSON_Delete(reply);
        if (!hosts) {
            pj_fail(r->err, "no host list in the router's data");
            cJSON_Delete(devices);
            return NULL;
        }
        int connected_count = cJSON_GetArraySize(devices);  // only the connected ones count as "connected"
        cJSON *h;
        cJSON_ArrayForEach(h, hosts) {
            if (pj_truthy(pj_get(h, "active"))) continue;
            const char *mac = cJSON_GetStringValue(pj_get(h, "mac"));
            int known = 0;
            for (int i = 0; mac && i < connected_count && !known; i++) {
                const char *dmac = cJSON_GetStringValue(pj_get(cJSON_GetArrayItem(devices, i), "mac"));
                known = dmac && same_mac(mac, dmac);
            }
            if (known) continue;
            cJSON *offline = cJSON_Duplicate(h, 1);
            pj_set(offline, "connection", cJSON_CreateString("offline"));  // {**h, "connection": "offline"}
            cJSON_AddItemToArray(devices, offline);
        }
        cJSON_Delete(hosts);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "count", cJSON_GetArraySize(devices));
    cJSON_AddItemToObject(out, "devices", devices);
    return out;
}

static cJSON *list_extenders(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    cJSON *extenders = mesh ? mesh_extenders(mesh) : NULL;
    cJSON_Delete(mesh);
    if (!extenders) {
        pj_fail(r->err, "no mesh in the router's data");
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "extenders", extenders);
    return out;
}

static cJSON *wan_details(const cJSON *args, router_t *r)
{
    (void)args;
    pj_err_t *err = r->err;
    WITH(ipv4_reply, "/api/v1/wan/ipv4");
    WITH(lan_reply, "/api/v1/lan/stats");
    WITH(status_reply, "/api/v1/wan/status");
    WITH(stats_reply, "/api/v1/wan/ip/stats");
    cJSON *out = NULL;
    if (!err->failed) {
        cJSON *ipv4 = wan_ipv4(ipv4_reply, err), *ports = lan_ports(lan_reply, err);
        // address.startswith("100.") and 64 <= int(address.split(".")[1]) <= 127
        const cJSON *address = pj_get(ipv4, "address");
        int cgnat = 0;
        if (!cJSON_IsString(address)) {
            pj_fail(err, "WAN address is not a string");
        } else if (strncmp(address->valuestring, "100.", 4) == 0) {
            char second[24];
            snprintf(second, sizeof(second), "%.*s", (int)strcspn(address->valuestring + 4, "."), address->valuestring + 4);
            cJSON *s = cJSON_CreateString(second), *n = pj_int(s, err);
            cgnat = n && n->valuedouble >= 64 && n->valuedouble <= 127;
            cJSON_Delete(s);
            cJSON_Delete(n);
        }
        const cJSON *p, *wan_port = NULL;
        cJSON_ArrayForEach(p, ports) {
            if (pj_is_str(pj_get(p, "role"), "WAN")) { wan_port = p; break; }
        }
        out = cJSON_CreateObject();
        add(out, "ipv4", ipv4);
        cJSON_AddBoolToObject(out, "cgnat", cgnat);
        add(out, "status", wan_status(status_reply, err));
        add(out, "traffic", wan_stats(stats_reply, err));
        add(out, "port", pj_dup(wan_port));
        cJSON_Delete(ports);
    }
    cJSON_Delete(ipv4_reply);
    cJSON_Delete(lan_reply);
    cJSON_Delete(status_reply);
    cJSON_Delete(stats_reply);
    return out;
}

static cJSON *ethernet_ports(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(lan_reply, "/api/v1/lan/stats");
    cJSON *out = NULL;
    if (lan_reply) {
        out = cJSON_CreateObject();
        add(out, "ports", lan_ports(lan_reply, r->err));
    }
    cJSON_Delete(lan_reply);
    return out;
}

static cJSON *wifi_details(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(home, "/api/v2/home");
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    WITH(steering, "/api/v1/wireless/bandsteering");
    WITH(mlo, "/api/v2/wireless/mlo/state");
    cJSON *out = NULL;
    if (!r->err->failed) {
        out = wifi_config(home, mesh, steering, mlo, r->err);  // {**wifi_config(), "gateway_radio_stats": ...}
        add(out, "gateway_radio_stats", wifi_stats(r));
    }
    cJSON_Delete(home);
    cJSON_Delete(mesh);
    cJSON_Delete(steering);
    cJSON_Delete(mlo);
    return out;
}

static cJSON *firewall_details(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(fw, "/api/v2/firewall");
    WITH(chain, "/api/v2/firewall/chain?chain=Custom");
    cJSON *out = r->err->failed ? NULL : firewall_settings(fw, chain, r->err);
    cJSON_Delete(fw);
    cJSON_Delete(chain);
    return out;
}

static cJSON *dhcp_details(const cJSON *args, router_t *r)
{
    (void)args;
    WITH(dhcp_reply, "/api/v1/dhcp");
    WITH(hosts_reply, "/api/v1/hosts");
    cJSON *out = NULL;
    if (!r->err->failed) {
        cJSON *hosts = mesh_hosts(hosts_reply);
        if (!hosts) pj_fail(r->err, "no host list in the router's data");
        out = cJSON_CreateObject();
        add(out, "server", dhcp(dhcp_reply, r->err));
        add(out, "hosts", hosts);
    }
    cJSON_Delete(dhcp_reply);
    cJSON_Delete(hosts_reply);
    return out;
}

// definitions generated from the Python server's tools/list (see test/e2e_mcp.py)
static const tool_t TOOLS[] = {
    {"router_overview",
     "Identity and health at a glance: model, serial, firmware, uptime,\n"
     "WAN status/IP, clock sync, number of extenders and clients.",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"router_overviewArguments\"}",
     router_overview},
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
     "{\"type\":\"object\",\"properties\":{\"include_inactive\":{\"default\":false,\"title\":\"Include Inactive\",\"type\":\"boolean\"}},\"title\":\"list_devicesArguments\"}",
     list_devices},
    {"list_extenders",
     "Mesh extenders: model, firmware, uptime, parent node, and backhaul\n"
     "(Ethernet speed, or per-band wifi signal/quality/channel).",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"list_extendersArguments\"}",
     list_extenders},
    {"wan_details",
     "Internet uplink: status, time since last change, public IPv4\n"
     "(flags CGNAT), gateway, WAN port speed and traffic/error counters.",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"wan_detailsArguments\"}",
     wan_details},
    {"ethernet_ports",
     "The gateway's physical Ethernet ports: link status, negotiated speed\n"
     "and duplex, rx/tx counters including errors and discards.",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"ethernet_portsArguments\"}",
     ethernet_ports},
    {"wifi_details",
     "Wifi setup: SSIDs per band (status, security; no passwords), channel\n"
     "per band per mesh node, band steering and MLO switches, and the\n"
     "gateway's per-band traffic/error counters.",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"wifi_detailsArguments\"}",
     wifi_details},
    {"firewall_details",
     "Firewall level, port-scan/fragment protection, and the custom rule\n"
     "chain (per rule: action, IPv4/IPv6, protocol, ports, direction).",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"firewall_detailsArguments\"}",
     firewall_details},
    {"dhcp_details",
     "LAN DHCP server config (pool, lease time, router IP) and every host\n"
     "the router knows with its IPv4/IPv6 addresses and lease state.",
     "{\"type\":\"object\",\"properties\":{},\"title\":\"dhcp_detailsArguments\"}",
     dhcp_details},
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
    pj_err_t e = {.msg = err, .size = err_size};
    err[0] = '\0';
    router_t r = {.fetch = fetch, .ctx = ctx, .err = &e};
    cJSON *out = tool->run(args, &r);
    if (e.failed || !out) {
        if (!e.failed) snprintf(err, err_size, "no result");
        cJSON_Delete(out);
        return NULL;
    }
    mesh_add_nicknames(out, nicknames, nickname_count);
    return out;
}
