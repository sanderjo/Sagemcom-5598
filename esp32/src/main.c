// Wifi + router login test. Row 0 of the LED matrix shows progress per step
// (col 0 crypt self-test, 1 wifi, 2 router login, 3 WAN data):
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
#include "router.h"
#include "secrets.h"
#include "sha512crypt.h"
#include "wifi.h"

#define LEVEL 5  // the LEDs are very bright

enum { STEP_CRYPT, STEP_WIFI, STEP_LOGIN, STEP_WAN };
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
    router_logout();
    ESP_LOGI(TAG, "logged out; done");
}
