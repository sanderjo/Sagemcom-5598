// Sagemcom 5598 MCP server on an ESP32-S3. Row 0 of the LED matrix shows
// status (col 0 crypt self-test, 1 wifi, 2 router login at boot, 3 MCP
// server, 4 last tool call): yellow = busy, green = OK, red = failed.
// Row 3: left blinks blue while an MCP request is handled, right flashes
// green for every connection to the router. Top right: heartbeat, a short
// green flash every 2 s.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mdns.h"
#include "activity.h"
#include "matrix.h"
#include "mcp_http.h"
#include "nicknames.h"
#include "router.h"
#include "secrets.h"
#include "sha512crypt.h"
#include "tools.h"
#include "wifi.h"

#define LEVEL 5  // the LEDs are very bright
#define HOSTNAME "sagemcom-mcp"

enum { STEP_CRYPT, STEP_WIFI, STEP_LOGIN, STEP_SERVER, STEP_TOOL };
enum { BUSY, OK, FAILED };

static const char *TAG = "main";

static void status(int step, int state)
{
    if (state == BUSY) matrix_set(0, step, LEVEL, LEVEL, 0);
    else if (state == OK) matrix_set(0, step, 0, LEVEL, 0);
    else matrix_set(0, step, LEVEL, 0, 0);
    matrix_show();
}

static void on_tool(const char *name, int state)
{
    status(STEP_TOOL, state);
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

// one login at boot, so a wrong password shows up on the matrix right away
static bool router_check(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = router_login(ROUTER_IP, ROUTER_LOGIN, ROUTER_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "router login failed: %s", err == ESP_ERR_INVALID_RESPONSE ? "password is incorrect" : esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "router login OK (%d ms)", (int)((esp_timer_get_time() - t0) / 1000));
    router_logout();
    return true;
}

// cJSON trees (tool results of up to a few hundred KB) go to PSRAM only. No
// fallback to internal RAM: a too-large result once used that up and the wifi
// driver aborted on its next allocation. Failures are counted, so the running
// tool reports "too large" instead of returning a result cut short.
static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) tools_alloc_failures++;
    return p;
}

// the event log tools need the real time (their window, pre-NTP router timestamps)
static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
    esp_netif_sntp_init(&cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) ESP_LOGI(TAG, "clock set by NTP");
    else ESP_LOGW(TAG, "no NTP time yet; event log tools wait for it");
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS failed to start");
        return;
    }
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("Sagemcom 5598 MCP server");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", HOSTNAME);
}

void app_main(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "ESP32-S3: flash %lu KB, PSRAM free %u KB", (unsigned long)(flash_size / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = psram_malloc, .free_fn = free});
    matrix_init();
    activity_start();
    router_set_request_hook(activity_router);

    status(STEP_CRYPT, BUSY);
    status(STEP_CRYPT, crypt_self_test() ? OK : FAILED);

    status(STEP_WIFI, BUSY);
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000) != ESP_OK) {
        // keeps retrying in the background; the server starts anyway
        ESP_LOGE(TAG, "wifi: no IP address after 30 s");
        status(STEP_WIFI, FAILED);
    } else {
        status(STEP_WIFI, OK);
    }
    start_mdns();
    start_sntp();  // keeps trying in the background if it can't sync now

    status(STEP_LOGIN, BUSY);
    status(STEP_LOGIN, wifi_is_connected() && router_check() ? OK : FAILED);

    static const mcp_http_config_t mcp = {
        .token = MCP_TOKEN,
        .router_ip = ROUTER_IP,
        .router_login = ROUTER_LOGIN,
        .router_password = ROUTER_PASSWORD,
        .nicknames = NICKNAMES,
        .nickname_count = NICKNAME_COUNT,
        .on_tool = on_tool,
        .on_request = activity_mcp,
    };
    status(STEP_SERVER, BUSY);
    status(STEP_SERVER, mcp_http_start(&mcp) == ESP_OK ? OK : FAILED);
}
