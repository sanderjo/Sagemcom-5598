#include "router.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sha512crypt.h"

#define MAX_BODY (256 * 1024)  // the 791 KB event log will need streaming later
#define MAX_COOKIES 8

static const char *TAG = "router";
static char s_base[48];
static char s_cookies[MAX_COOKIES][160];  // "name=value"
static void (*s_request_hook)(void);

void router_set_request_hook(void (*hook)(void))
{
    s_request_hook = hook;
}

typedef struct {
    router_resp_t *resp;
    size_t cap;
    bool overflow;
} ctx_t;

static void store_cookie(const char *set_cookie)
{
    size_t n = strcspn(set_cookie, ";");
    const char *eq = memchr(set_cookie, '=', n);
    if (!eq || n >= sizeof(s_cookies[0])) return;
    size_t name_len = eq - set_cookie + 1;  // including '='
    int slot = -1;
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (strncmp(s_cookies[i], set_cookie, name_len) == 0) { slot = i; break; }
        if (slot < 0 && !s_cookies[i][0]) slot = i;
    }
    if (slot < 0) return;
    memcpy(s_cookies[slot], set_cookie, n);
    s_cookies[slot][n] = '\0';
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Set-Cookie") == 0) {
        store_cookie(evt->header_value);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && !ctx->overflow) {
        router_resp_t *r = ctx->resp;
        if (r->len + evt->data_len + 1 > ctx->cap) {
            size_t cap = ctx->cap ? ctx->cap * 2 : 4096;
            while (cap < r->len + evt->data_len + 1) cap *= 2;
            char *grown = cap <= MAX_BODY ? heap_caps_realloc(r->body, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!grown) { ctx->overflow = true; return ESP_OK; }
            r->body = grown;
            ctx->cap = cap;
        }
        memcpy(r->body + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
        r->body[r->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t request(esp_http_client_method_t method, const char *path, const char *form, router_resp_t *resp)
{
    char url[128], cookie_header[MAX_COOKIES * 162] = "", referer[64];
    if (s_request_hook) s_request_hook();
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    snprintf(referer, sizeof(referer), "%s/", s_base);
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (!s_cookies[i][0]) continue;
        if (cookie_header[0]) strlcat(cookie_header, "; ", sizeof(cookie_header));
        strlcat(cookie_header, s_cookies[i], sizeof(cookie_header));
    }

    *resp = (router_resp_t){0};
    ctx_t ctx = {.resp = resp};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .event_handler = on_http_event,
        .user_data = &ctx,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Referer", referer);
    esp_http_client_set_header(client, "X-CSRF-Token", "");
    if (cookie_header[0]) esp_http_client_set_header(client, "Cookie", cookie_header);
    if (form) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, form, strlen(form));
    }

    esp_err_t err = esp_http_client_perform(client);
    resp->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && ctx.overflow) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s %s: %s", form ? "POST" : "GET", path, esp_err_to_name(err));
        router_free(resp);
    }
    return err;
}

esp_err_t router_get(const char *path, router_resp_t *resp)
{
    return request(HTTP_METHOD_GET, path, NULL, resp);
}

void router_free(router_resp_t *resp)
{
    heap_caps_free(resp->body);
    *resp = (router_resp_t){0};
}

// login names and nonces are plain ASCII here, but encode anyway
static void form_escape(const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (; *in && o + 4 < size; in++) {
        unsigned char c = *in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
            out[o++] = c;
        else
            o += snprintf(out + o, size - o, "%%%02X", c);
    }
    out[o] = '\0';
}

esp_err_t router_login(const char *ip, const char *login, const char *password)
{
    snprintf(s_base, sizeof(s_base), "http://%s", ip);
    memset(s_cookies, 0, sizeof(s_cookies));
    router_resp_t r;
    char form[512], login_esc[96];
    form_escape(login, login_esc, sizeof(login_esc));

    esp_err_t err = request(HTTP_METHOD_GET, "/api/v1/open", NULL, &r);
    if (err != ESP_OK) return err;
    router_free(&r);

    snprintf(form, sizeof(form), "login=%s", login_esc);
    if ((err = request(HTTP_METHOD_POST, "/api/v2/login-params", form, &r)) != ESP_OK) return err;
    cJSON *json = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    cJSON *params = cJSON_GetArrayItem(json, 0);
    const char *salt = cJSON_GetStringValue(cJSON_GetObjectItem(params, "salt"));
    const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(params, "nonce"));
    if (!salt || !nonce) {
        ESP_LOGE(TAG, "login-params: HTTP %d, no salt/nonce", r.status);
        cJSON_Delete(json);
        router_free(&r);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char crypt_hash[SHA512_CRYPT_MAX], step2[SHA512_HEX_LEN + 1], auth_key[SHA512_HEX_LEN + 1], cnonce[20];
    char text[320];
    err = sha512_crypt(password, salt, crypt_hash);
    snprintf(text, sizeof(text), "%s:%s:%s", login, nonce, crypt_hash);
    cJSON_Delete(json);
    router_free(&r);
    if (err != ESP_OK || sha512_hex(text, step2) != ESP_OK) return ESP_FAIL;

    // 19 random decimal digits, like the GUI
    for (int i = 0; i < 19; i++) cnonce[i] = '0' + esp_random() % 10;
    cnonce[19] = '\0';
    snprintf(text, sizeof(text), "%s:0:%s", step2, cnonce);
    if (sha512_hex(text, auth_key) != ESP_OK) return ESP_FAIL;

    snprintf(form, sizeof(form), "login=%s&auth_key=%s&cnonce=%s", login_esc, auth_key, cnonce);
    if ((err = request(HTTP_METHOD_POST, "/api/v1/login", form, &r)) != ESP_OK) return err;
    int status = r.status;
    router_free(&r);
    if (status == 400) return ESP_ERR_INVALID_RESPONSE;
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "login: HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void router_logout(void)
{
    router_resp_t r;
    if (request(HTTP_METHOD_POST, "/api/v1/logout", "", &r) == ESP_OK) router_free(&r);
}
