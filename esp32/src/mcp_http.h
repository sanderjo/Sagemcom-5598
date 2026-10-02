// MCP Streamable HTTP endpoint (POST /mcp, JSON responses, no SSE) on port 80.
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "mesh.h"

typedef struct {
    const char *token;  // required "Authorization: Bearer <token>"
    const char *router_ip, *router_login, *router_password;
    const nickname_t *nicknames;
    size_t nickname_count;
    // state 0 = busy, 1 = ok, 2 = failed (for the LED matrix); may be NULL
    void (*on_tool)(const char *name, int state);
    // 1 when an authorized MCP request starts, 0 when it is answered; may be NULL
    void (*on_request)(int begin);
} mcp_http_config_t;

esp_err_t mcp_http_start(const mcp_http_config_t *config);
