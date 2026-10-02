// Client for the Sagemcom F@st 5598 web API (port of sagemcom5598.py's login flow).
#pragma once
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    int status;   // HTTP status code
    char *body;   // NUL-terminated, in PSRAM; free with router_free()
    size_t len;
} router_resp_t;

// ESP_ERR_INVALID_RESPONSE: wrong password (HTTP 400), like the Python CLI reports it.
esp_err_t router_login(const char *ip, const char *login, const char *password);
esp_err_t router_get(const char *path, router_resp_t *resp);
void router_logout(void);
// called at the start of every HTTP request to the router (e.g. for an activity LED)
void router_set_request_hook(void (*hook)(void));
void router_free(router_resp_t *resp);
