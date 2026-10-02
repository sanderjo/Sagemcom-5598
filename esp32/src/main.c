// Wifi + router login test. Row 0 of the LED matrix shows progress per step
// (col 0 crypt self-test, 1 wifi, 2 router login, 3 WAN data, 4 mesh views):
// yellow = busy, green = OK, red = failed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "matrix.h"
#include "mesh.h"
#include "nicknames.h"
#include "router.h"
#include "secrets.h"
#include "sha512crypt.h"
#include "wifi.h"

#define LEVEL 5  // the LEDs are very bright

enum { STEP_CRYPT, STEP_WIFI, STEP_LOGIN, STEP_WAN, STEP_MESH };
enum { BUSY, OK, FAILED };

static const char *TAG = "main";

static void status(int step, int state)
{
    if (state == BUSY) matrix_set(0, step, LEVEL, LEVEL, 0);
    else if (state == OK) matrix_set(0, step, 0, LEVEL, 0);
    else matrix_set(0, step, LEVEL, 0, 0);
    matrix_show();
}

static bool crypt_self_test(void)
{
    char out[SHA512_CRYPT_MAX];
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = sha512_crypt(CRYPT_TEST_PASSWORD, CRYPT_TEST_SALT, out);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    bool ok = err == ESP_OK && strcmp(out, CRYPT_TEST_EXPECTED) == 0;
    ESP_LOGI(TAG, "sha512-crypt self-test %s (%d ms)", ok ? "OK" : "FAILED", ms);
    if (!ok) ESP_LOGE(TAG, "got %s, expected %s", out, CRYPT_TEST_EXPECTED);
    return ok;
}

static void show_wan_ipv4(void)
{
    router_resp_t r;
    if (router_get("/api/v1/wan/ipv4", &r) != ESP_OK) { status(STEP_WAN, FAILED); return; }
    cJSON *json = cJSON_Parse(r.body);
    cJSON *ipv4 = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(json, 0), "wan"), "ipv4");
    const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(ipv4, "address"));
    if (r.status == 200 && addr) {
        cJSON *uptime = cJSON_GetObjectItem(ipv4, "uptime");  // a number, or a numeric string
        ESP_LOGI(TAG, "WAN: status %s, address %s, gateway %s, uptime %.0f s (%u bytes)",
                 cJSON_GetStringValue(cJSON_GetObjectItem(ipv4, "status")), addr,
                 cJSON_GetStringValue(cJSON_GetObjectItem(ipv4, "gateway")),
                 cJSON_IsNumber(uptime) ? uptime->valuedouble : cJSON_IsString(uptime) ? atof(uptime->valuestring) : -1.0,
                 (unsigned)r.len);
        status(STEP_WAN, OK);
    } else {
        ESP_LOGE(TAG, "wan/ipv4: HTTP %d: %.200s", r.status, r.body ? r.body : "");
        status(STEP_WAN, FAILED);
    }
    cJSON_Delete(json);
    router_free(&r);
}

static const char *str(const cJSON *obj, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    return s ? s : "-";
}

// "name (nickname)" like the Python CLI's with_nickname()
static const char *named(const cJSON *obj, const char *key, const char *nick_key, char *buf, size_t size)
{
    const char *nick = cJSON_GetStringValue(cJSON_GetObjectItem(obj, nick_key));
    if (nick) snprintf(buf, size, "%s (%s)", str(obj, key), nick);
    else snprintf(buf, size, "%s", str(obj, key));
    return buf;
}

static void print_node(const cJSON *node, int depth)
{
    char name[96], client[96];
    const cJSON *c, *ext, *dbm = cJSON_GetObjectItem(node, "signal_strength_dbm");
    char *signal = cJSON_GetArraySize(dbm) ? cJSON_PrintUnformatted(dbm) : NULL;
    ESP_LOGI(TAG, "%*s%s %s %s%s%s", depth * 4, "", named(node, "hostname", "nickname", name, sizeof(name)),
             str(node, "ipv4"), cJSON_IsString(cJSON_GetObjectItem(node, "backhaul_type")) ? str(node, "backhaul_type") : "",
             signal ? " " : "", signal ? signal : "");
    cJSON_free(signal);
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(node, "clients")) {
        const cJSON *dbm_c = cJSON_GetObjectItem(c, "signal_strength");
        if (cJSON_IsNumber(dbm_c))
            ESP_LOGI(TAG, "%*s  - %s %s, %s GHz %d dBm", depth * 4, "", named(c, "name", "nickname", client, sizeof(client)),
                     str(c, "ip"), str(c, "band"), dbm_c->valueint);
        else
            ESP_LOGI(TAG, "%*s  - %s %s, wired %d Mbps", depth * 4, "", named(c, "name", "nickname", client, sizeof(client)),
                     str(c, "ip"), cJSON_GetObjectItem(c, "link_speed_mbps") ? cJSON_GetObjectItem(c, "link_speed_mbps")->valueint : 0);
    }
    cJSON_ArrayForEach(ext, cJSON_GetObjectItem(node, "extenders")) print_node(ext, depth + 1);
}

static void show_mesh(void)
{
    router_resp_t r;
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int64_t t0 = esp_timer_get_time();
    if (router_get("/api/v4/easymesh/meshdevices", &r) != ESP_OK || r.status != 200) {
        ESP_LOGE(TAG, "meshdevices: HTTP %d", r.status);
        router_free(&r);
        status(STEP_MESH, FAILED);
        return;
    }
    int64_t t1 = esp_timer_get_time();
    cJSON *reply = cJSON_Parse(r.body);
    cJSON *extenders = mesh_extenders(reply), *devices = mesh_devices(reply), *topology = mesh_topology(reply);
    size_t heap_min = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (!extenders || !devices || !topology) {
        ESP_LOGE(TAG, "meshdevices: no mesh in reply (%u bytes)", (unsigned)r.len);
        status(STEP_MESH, FAILED);
    } else {
        mesh_add_nicknames(extenders, NICKNAMES, NICKNAME_COUNT);
        mesh_add_nicknames(devices, NICKNAMES, NICKNAME_COUNT);
        mesh_add_nicknames(topology, NICKNAMES, NICKNAME_COUNT);
        ESP_LOGI(TAG, "meshdevices: %u bytes, fetched in %d ms, views built in %d ms, %u KB heap in use",
                 (unsigned)r.len, (int)((t1 - t0) / 1000), (int)((esp_timer_get_time() - t1) / 1000),
                 (unsigned)((heap_before - heap_min) / 1024));
        ESP_LOGI(TAG, "%d extenders, %d devices; topology:", cJSON_GetArraySize(extenders), cJSON_GetArraySize(devices));
        print_node(topology, 0);
        status(STEP_MESH, OK);
    }
    cJSON_Delete(topology);
    cJSON_Delete(devices);
    cJSON_Delete(extenders);
    cJSON_Delete(reply);
    router_free(&r);
}

void app_main(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "ESP32-S3: flash %lu KB, PSRAM free %u KB", (unsigned long)(flash_size / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    matrix_init();

    status(STEP_CRYPT, BUSY);
    status(STEP_CRYPT, crypt_self_test() ? OK : FAILED);

    status(STEP_WIFI, BUSY);
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000) != ESP_OK) {
        ESP_LOGE(TAG, "wifi: no IP address after 30 s");
        status(STEP_WIFI, FAILED);
        return;
    }
    status(STEP_WIFI, OK);

    status(STEP_LOGIN, BUSY);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = router_login(ROUTER_IP, ROUTER_LOGIN, ROUTER_PASSWORD);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "router login failed: %s", err == ESP_ERR_INVALID_RESPONSE ? "password is incorrect" : esp_err_to_name(err));
        status(STEP_LOGIN, FAILED);
        return;
    }
    ESP_LOGI(TAG, "router login OK (%d ms)", ms);
    status(STEP_LOGIN, OK);

    status(STEP_WAN, BUSY);
    show_wan_ipv4();
    status(STEP_MESH, BUSY);
    show_mesh();
    router_logout();
    ESP_LOGI(TAG, "logged out; done");
}
