// MCP tools, mirroring sagemcom5598_mcp.py. Plain cJSON, no ESP-IDF: router
// data comes in through `fetch`, so this also builds on the host (see test/).
#pragma once
#include <stddef.h>
#include "cJSON.h"
#include "mesh.h"

// GET `path` on the router and return the parsed JSON, or NULL with a
// message in `err` (e.g. "No router reachable at 192.168.1.254").
typedef cJSON *(*tools_fetch_fn)(void *ctx, const char *path, char *err, size_t err_size);

// Tool definitions for tools/list (new array).
cJSON *tools_definitions(void);
int tools_exists(const char *name);
// Run a tool: its result object (new), or NULL with a message in `err`.
cJSON *tools_run(const char *name, const cJSON *args, tools_fetch_fn fetch, void *ctx,
                 const nickname_t *nicknames, size_t nickname_count, char *err, size_t err_size);
