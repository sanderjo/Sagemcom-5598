#include "tools.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eventlog.h"
#include "pyjson.h"

volatile unsigned tools_alloc_failures;

typedef struct {
    const tools_env_t *env;
    pj_err_t *err;
    const nickname_t *nicknames;
    size_t nickname_count;
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
    cJSON *json = r->env->fetch(r->env->ctx, path, msg, sizeof(msg));
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

// --- event log tools -----------------------------------------------------------

// Argument checks like the Python server's (pydantic): NULL value = not given.
static int arg_number(const cJSON *args, const char *key, double dflt, double *out, pj_err_t *err)
{
    const cJSON *v = pj_get(args, key);
    *out = dflt;
    if (!v || cJSON_IsNull(v)) return 1;
    if (!cJSON_IsNumber(v)) { pj_fail(err, "argument '%s' must be a number", key); return 0; }
    *out = v->valuedouble;
    return 1;
}

static int arg_string(const cJSON *args, const char *key, const char **out, pj_err_t *err)
{
    const cJSON *v = pj_get(args, key);
    *out = NULL;
    if (!v || cJSON_IsNull(v)) return 1;
    if (!cJSON_IsString(v)) { pj_fail(err, "argument '%s' must be a string or null", key); return 0; }
    *out = v->valuestring;
    return 1;
}

static int64_t now_us(router_t *r)
{
    int64_t now = r->env->now_us ? r->env->now_us(r->env->ctx) : -1;
    if (now < 0) pj_fail(r->err, "the clock is not set yet (no NTP sync), so the event window can't be computed");
    return now;
}

// What both log tools need: the parsed log, and Names from hosts and the mesh.
typedef struct {
    char *raw;
    event_log_t *log;
    cJSON *hosts, *extenders, *gateway, *devices;
    names_t *names;
    int64_t now;
} log_state_t;

static void log_state_free(log_state_t *s)
{
    names_free(s->names);
    eventlog_free(s->log);
    free(s->raw);
    cJSON_Delete(s->hosts);
    cJSON_Delete(s->extenders);
    cJSON_Delete(s->gateway);
    cJSON_Delete(s->devices);
}

static int load_log(router_t *r, log_state_t *s)
{
    pj_err_t *err = r->err;
    *s = (log_state_t){0};
    if ((s->now = now_us(r)) < 0) return 0;
    char msg[160] = "";
    size_t len = 0;
    s->raw = r->env->fetch_raw(r->env->ctx, "/api/v1/device/log", &len, msg, sizeof(msg));
    if (!s->raw) { pj_fail(err, "%s", msg[0] ? msg : "router request failed"); return 0; }
    WITH(open, "/api/v1/open");
    cJSON *info = open ? device_info(open, err) : NULL;
    cJSON_Delete(open);
    const cJSON *uptime = pj_get(info, "uptime");
    if (!err->failed && cJSON_IsNumber(uptime))
        s->log = eventlog_parse(s->raw, len, s->now - (int64_t)llround(uptime->valuedouble * 1e6), err);
    cJSON_Delete(info);
    if (!s->log) return 0;

    WITH(hosts_reply, "/api/v1/hosts");
    WITH(mesh, "/api/v4/easymesh/meshdevices");
    if (!err->failed) {
        s->hosts = mesh_hosts(hosts_reply);
        if (!s->hosts) pj_fail(err, "no host list in the router's data");
        s->extenders = mesh_extenders(mesh);
        s->gateway = gateway_node(mesh, err);
        s->devices = mesh_devices(mesh);
        if (!s->extenders || !s->devices) pj_fail(err, "no mesh in the router's data");
    }
    cJSON_Delete(hosts_reply);
    cJSON_Delete(mesh);
    if (!err->failed) s->names = names_build(s->hosts, s->extenders, s->gateway, s->devices, err);
    return !err->failed;
}

static char *ascii_lower(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out)
        for (size_t i = 0; i <= n; i++) out[i] = tolower((unsigned char)s[i]);
    return out;
}

static cJSON *event_log(const cJSON *args, router_t *r)
{
    double hours, limit_d;
    const char *module, *level, *device, *contains;
    if (!arg_number(args, "hours", 25, &hours, r->err) || !arg_string(args, "module", &module, r->err) ||
        !arg_string(args, "level", &level, r->err) || !arg_string(args, "device", &device, r->err) ||
        !arg_string(args, "contains", &contains, r->err) || !arg_number(args, "limit", 200, &limit_d, r->err))
        return NULL;
    if (limit_d != (double)(long long)limit_d) {
        pj_fail(r->err, "argument 'limit' must be an integer");
        return NULL;
    }
    // _resolve_nickname(device).lower(): a nickname stands for its hostname/MAC
    char *needle = NULL;
    if (device && *device) {
        char *dev = ascii_lower(device);
        const char *name = dev;
        for (size_t i = 0; dev && i < r->nickname_count; i++) {
            char *nick = ascii_lower(r->nicknames[i].nickname);
            if (nick && !strcmp(nick, dev)) name = r->nicknames[i].name;  // the last one wins, like a dict
            free(nick);
        }
        needle = name ? ascii_lower(name) : NULL;
        free(dev);
    }
    char *contains_l = contains && *contains ? ascii_lower(contains) : NULL;

    log_state_t s;
    cJSON *out = NULL;
    if (load_log(r, &s))
        out = eventlog_query(s.log, s.names, s.now, hours, module, level, needle, contains_l, (long long)limit_d, r->err);
    log_state_free(&s);
    free(needle);
    free(contains_l);
    return out;
}

static cJSON *event_summary(const cJSON *args, router_t *r)
{
    double hours;
    if (!arg_number(args, "hours", 25, &hours, r->err)) return NULL;
    const cJSON *given = pj_get(args, "hours");
    cJSON *hours_json = cJSON_IsNumber(given) ? cJSON_Duplicate(given, 1) : cJSON_CreateNumber(25);
    log_state_t s;
    cJSON *out = NULL;
    if (load_log(r, &s))
        out = eventlog_summary(s.log, s.names, s.now, hours_json, r->env->own_ip ? r->env->own_ip(r->env->ctx) : NULL,
                               r->err);
    log_state_free(&s);
    cJSON_Delete(hours_json);
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
    {"event_log",
     "The router's own event log for the last `hours`, newest first.\n"
     "Filters: `module` (WIFI, SYS, GUI, DNS, DHCPC, DHCPS, WETH=WAN Ethernet,\n"
     "LETH=LAN Ethernet), `level` (info, warning, err), `device` (MAC or part\n"
     "of a hostname), `contains` (text). MACs are annotated with device names.",
     "{\"type\":\"object\",\"properties\":{\"hours\":{\"default\":25,\"title\":\"Hours\",\"type\":\"number\"},\"module\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}],\"default\":null,\"title\":\"Module\"},\"level\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}],\"default\":null,\"title\":\"Level\"},\"device\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}],\"default\":null,\"title\":\"Device\"},\"contains\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}],\"default\":null,\"title\":\"Contains\"},\"limit\":{\"default\":200,\"title\":\"Limit\",\"type\":\"integer\"}},\"title\":\"event_logArguments\"}",
     event_log},
    {"event_summary",
     "The event log over the last `hours` condensed: counts per event kind,\n"
     "per wifi device (connects, disconnects, failed logins, SSIDs), GUI admin\n"
     "logins by source IP, and any uncategorized events.",
     "{\"type\":\"object\",\"properties\":{\"hours\":{\"default\":25,\"title\":\"Hours\",\"type\":\"number\"}},\"title\":\"event_summaryArguments\"}",
     event_summary},
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

cJSON *tools_run(const char *name, const cJSON *args, const tools_env_t *env,
                 const nickname_t *nicknames, size_t nickname_count, char *err, size_t err_size)
{
    const tool_t *tool = find(name);
    if (!tool) {
        snprintf(err, err_size, "Unknown tool: %s", name ? name : "(none)");
        return NULL;
    }
    pj_err_t e = {.msg = err, .size = err_size};
    err[0] = '\0';
    router_t r = {.env = env, .err = &e, .nicknames = nicknames, .nickname_count = nickname_count};
    unsigned failures_before = tools_alloc_failures;
    cJSON *out = tool->run(args, &r);
    if (out) mesh_add_nicknames(out, nicknames, nickname_count);
    if (tools_alloc_failures != failures_before) {
        cJSON_Delete(out);
        snprintf(err, err_size, "the result is too large for the ESP32's memory; ask for less "
                                "(a smaller `limit`, fewer `hours` or a filter)");
        return NULL;
    }
    if (e.failed || !out) {
        if (!e.failed) snprintf(err, err_size, "no result");
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}
