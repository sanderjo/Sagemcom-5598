// MCP over JSON-RPC (handshake protocol versions 2024-11-05 .. 2025-11-25).
// Plain cJSON, no ESP-IDF, so it also builds on the host (see test/).
#pragma once
#include "cJSON.h"
#include "tools.h"

typedef struct {
    tools_fetch_fn fetch;
    void *fetch_ctx;
    const nickname_t *nicknames;
    size_t nickname_count;
    // called around each tool run (e.g. to log in/out of the router, update LEDs); may be NULL
    void (*before_tool)(void *ctx, const char *name);
    void (*after_tool)(void *ctx, const char *name, int ok);
} mcp_server_t;

// Handle one parsed JSON-RPC message or batch. Returns the response (new),
// or NULL when nothing must be sent back (only notifications).
cJSON *mcp_handle(const mcp_server_t *server, const cJSON *message);
// A JSON-RPC error response with id null (e.g. for unparsable input).
cJSON *mcp_error(int code, const char *message);

#define MCP_PARSE_ERROR -32700
#define MCP_INVALID_REQUEST -32600
#define MCP_METHOD_NOT_FOUND -32601
#define MCP_INVALID_PARAMS -32602
