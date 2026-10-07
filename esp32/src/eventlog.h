// The router's event log (/api/v1/device/log, ~800 KB), ported from
// sagemcom5598.py (event_log) and sagemcom5598_diagnose.py (classify, Names,
// events_in_window, summarize_events). The raw reply is scanned in place,
// without building a cJSON tree for it. Plain C + cJSON, host-testable.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "pyjson.h"

typedef struct event_log event_log_t;
typedef struct names names_t;

// Parse the raw reply (modified in place; keep it alive while the log is
// used). `boot_us` = now - device uptime, for entries with a pre-NTP clock.
event_log_t *eventlog_parse(char *raw, size_t len, int64_t boot_us, pj_err_t *err);
void eventlog_free(event_log_t *log);

// Names(hosts, extenders, gateway id/name, devices) from the client views.
names_t *names_build(const cJSON *hosts, const cJSON *extenders, const cJSON *gateway, const cJSON *devices,
                     pj_err_t *err);
void names_free(names_t *names);

// The event_log tool's result. Optional filters are NULL when not given;
// `device_needle` is already nickname-resolved and lowercase.
cJSON *eventlog_query(const event_log_t *log, const names_t *names, int64_t now_us, double hours,
                      const char *module, const char *level, const char *device_needle, const char *contains,
                      long long limit, pj_err_t *err);
// The event_summary tool's result. `hours` is echoed as given.
cJSON *eventlog_summary(const event_log_t *log, const names_t *names, int64_t now_us, const cJSON *hours,
                        const char *own_ip, pj_err_t *err);
