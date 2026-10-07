// MCP tools, mirroring sagemcom5598_mcp.py. Plain cJSON, no ESP-IDF: router
// data comes in through `fetch`, so this also builds on the host (see test/).
#pragma once
#include <stddef.h>
#include "cJSON.h"
#include "mesh.h"

#include <stdint.h>

// GET `path` on the router and return the parsed JSON, or NULL with a
// message in `err` (e.g. "No router reachable at 192.168.1.254").
typedef cJSON *(*tools_fetch_fn)(void *ctx, const char *path, char *err, size_t err_size);
// GET `path` and return the raw body (NUL-terminated, the caller free()s it),
// for replies too large for a cJSON tree (the event log).
typedef char *(*tools_fetch_raw_fn)(void *ctx, const char *path, size_t *len, char *err, size_t err_size);

typedef struct {
    tools_fetch_fn fetch;
    tools_fetch_raw_fn fetch_raw;
    void *ctx;
    int64_t (*now_us)(void *ctx);         // wall clock, UTC microseconds; < 0 = not known yet
    const char *(*own_ip)(void *ctx);     // our IPv4 as the router sees it; may return NULL
} tools_env_t;

// Incremented by the allocator the firmware gives cJSON when an allocation
// fails; tools_run() turns any failure during a tool into an error, so a
// result is never silently cut short (cJSON drops items it can't allocate).
extern volatile unsigned tools_alloc_failures;

// Tool definitions for tools/list (new array).
cJSON *tools_definitions(void);
int tools_exists(const char *name);
// Run a tool: its result object (new), or NULL with a message in `err`.
cJSON *tools_run(const char *name, const cJSON *args, const tools_env_t *env,
                 const nickname_t *nicknames, size_t nickname_count, char *err, size_t err_size);
