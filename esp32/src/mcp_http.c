#include "mcp_http.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mcp_proto.h"
#include "router.h"

#define MAX_BODY (64 * 1024)

static const char *TAG = "mcp";

typedef struct {
    const mcp_http_config_t *config;
    int logged_in;
} session_t;

static mcp_http_config_t s_config;
static session_t s_session;
static mcp_server_t s_server;

// --- router access for the tools: log in lazily, log out after each tool ---

static cJSON *fetch(void *ctx, const char *path, char *err, size_t err_size)
{
    session_t *s = ctx;
    if (!s->logged_in) {
        esp_err_t e = ESP_FAIL;
        for (int attempt = 0; attempt < 2; attempt++) {  // the router occasionally drops a connection
            e = router_login(s->config->router_ip, s->config->router_login, s->config->router_password);
            if (e == ESP_OK || e == ESP_ERR_INVALID_RESPONSE) break;
            if (!attempt) vTaskDelay(pdMS_TO_TICKS(3000));
        }
        if (e == ESP_ERR_INVALID_RESPONSE) {
            snprintf(err, err_size, "Router rejected the login (wrong password?)");
            return NULL;
        }
        if (e != ESP_OK) {
            snprintf(err, err_size, "No router reachable at %s", s->config->router_ip);
            return NULL;
        }
        s->logged_in = 1;
    }
    router_resp_t r;
    if (router_get(path, &r) != ESP_OK) {
        snprintf(err, err_size, "no answer from the router for %s", path);
        return NULL;
    }
    cJSON *json = NULL;
    if (r.status != 200) snprintf(err, err_size, "HTTP %d from %s", r.status, path);
    else if (!(json = cJSON_Parse(r.body))) snprintf(err, err_size, "invalid JSON from %s", path);
    router_free(&r);
    return json;
}

static void before_tool(void *ctx, const char *name)
{
    (void)ctx;
    if (s_config.on_tool) s_config.on_tool(name, 0);
}

static void after_tool(void *ctx, const char *name, int ok)
{
    session_t *s = ctx;
    if (s->logged_in) {
        router_logout();
        s->logged_in = 0;
    }
    if (s_config.on_tool) s_config.on_tool(name, ok ? 1 : 2);
}

// --- HTTP ---------------------------------------------------------------

// no early exit on the first differing byte (the length is not secret)
static int constant_time_equal(const char *a, const char *b)
{
    size_t len = strlen(a);
    if (len != strlen(b)) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = text ? httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN) : httpd_resp_send_500(req);
    cJSON_free(text);
    return e;
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static int authorized(httpd_req_t *req)
{
    char header[128], expected[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return 0;
    snprintf(expected, sizeof(expected), "Bearer %s", s_config.token);
    return constant_time_equal(header, expected);
}

// Browsers send Origin; accept only our own (DNS rebinding protection). Claude Code sends none.
static int origin_ok(httpd_req_t *req)
{
    char origin[128], host[96], own[112];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return 1;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return 0;
    snprintf(own, sizeof(own), "http://%s", host);
    return strcmp(origin, own) == 0;
}

static esp_err_t mcp_post(httpd_req_t *req)
{
    if (!origin_ok(req)) return send_text(req, "403 Forbidden", "origin not allowed\n");
    if (!authorized(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"sagemcom5598\"");
        return send_text(req, "401 Unauthorized", "missing or wrong bearer token\n");
    }
    if (req->content_len == 0 || req->content_len > MAX_BODY)
        return send_text(req, "413 Payload Too Large", "body must be 1..65536 bytes\n");

    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return httpd_resp_send_500(req);
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            heap_caps_free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    int64_t t0 = esp_timer_get_time();
    cJSON *msg = cJSON_Parse(body);
    heap_caps_free(body);
    if (!msg) {
        cJSON *err = mcp_error(MCP_PARSE_ERROR, "Parse error");
        esp_err_t e = send_json(req, "400 Bad Request", err);
        cJSON_Delete(err);
        return e;
    }
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));
    const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(msg, "params"), "name"));
    cJSON *reply = mcp_handle(&s_server, msg);
    ESP_LOGI(TAG, "%s%s%s: %s in %d ms", method ? method : "(batch)", tool ? " " : "", tool ? tool : "",
             reply ? "answered" : "accepted", (int)((esp_timer_get_time() - t0) / 1000));
    cJSON_Delete(msg);

    esp_err_t e;
    if (!reply) {
        httpd_resp_set_status(req, "202 Accepted");
        e = httpd_resp_send(req, NULL, 0);
    } else {
        e = send_json(req, "200 OK", reply);
        cJSON_Delete(reply);
    }
    return e;
}

static esp_err_t mcp_other(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Allow", "POST");
    return send_text(req, "405 Method Not Allowed", "this MCP server only takes POST (no SSE stream)\n");
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_text(req, "200 OK", "sagemcom5598 MCP server (ESP32-S3): POST /mcp with a bearer token\n");
}

esp_err_t mcp_http_start(const mcp_http_config_t *config)
{
    s_config = *config;
    s_session = (session_t){.config = &s_config};
    s_server = (mcp_server_t){
        .fetch = fetch,
        .fetch_ctx = &s_session,
        .nicknames = config->nicknames,
        .nickname_count = config->nickname_count,
        .before_tool = before_tool,
        .after_tool = after_tool,
    };

    httpd_handle_t httpd;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16 * 1024;  // tool runs (router client + cJSON) happen in the server task
    cfg.lru_purge_enable = true;
    // one server task: requests (and so router sessions) are handled one at a time
    esp_err_t e = httpd_start(&httpd, &cfg);
    if (e != ESP_OK) return e;
    const httpd_uri_t uris[] = {
        {.uri = "/mcp", .method = HTTP_POST, .handler = mcp_post},
        {.uri = "/mcp", .method = HTTP_GET, .handler = mcp_other},
        {.uri = "/mcp", .method = HTTP_DELETE, .handler = mcp_other},
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(httpd, &uris[i]);
    ESP_LOGI(TAG, "listening on port %d, POST /mcp", cfg.server_port);
    return ESP_OK;
}
