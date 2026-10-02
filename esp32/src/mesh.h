// Mesh views from /api/v4/easymesh/meshdevices, ported from sagemcom5598.py
// (connected_extenders, connected_devices, topology), hosts() from
// /api/v1/hosts, plus add_nicknames.
// Plain cJSON, no ESP-IDF, so it also builds on the host (see test/).
#pragma once
#include <stddef.h>
#include "cJSON.h"

typedef struct {
    const char *name;      // hostname or MAC, lowercase
    const char *nickname;
} nickname_t;

// `reply` is the parsed meshdevices reply (a one-element array). All return
// new cJSON trees the caller must cJSON_Delete; NULL if the reply has no mesh.
cJSON *mesh_extenders(const cJSON *reply);
cJSON *mesh_devices(const cJSON *reply);
cJSON *mesh_topology(const cJSON *reply);
// `reply` is the parsed /api/v1/hosts reply
cJSON *mesh_hosts(const cJSON *reply);

// Add "<field>_nickname" next to known hostnames/MACs, in place (like add_nicknames()).
void mesh_add_nicknames(cJSON *data, const nickname_t *nicknames, size_t count);
