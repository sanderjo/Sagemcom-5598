#include "eventlog.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define US 1000000LL
#define EPOCH_2013_US (1356998400LL * US)  // datetime(2013, 1, 1, tzinfo=utc)
#define MAX_DEPTH 64

// A field of a log entry: a string (pointing into the raw buffer), any
// other JSON value (parsed), or missing.
typedef struct {
    const char *s;
    uint32_t len;
    cJSON *json;
    uint8_t present;
} jval_t;

typedef struct {
    int64_t t_us;      // instant (UTC, microseconds), after clock correction
    int32_t offset_s;  // the entry's own UTC offset, for display
    uint32_t index;    // position in the router's list (stable sort)
    uint8_t corrected;
    jval_t level, module, message;
} event_t;

struct event_log {
    event_t *events;
    size_t count;
};

// --- minimal in-place JSON scanner ------------------------------------------

typedef struct {
    char *p, *end;
} sc_t;

static int peek(sc_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
    return s->p < s->end ? (unsigned char)*s->p : -1;
}

static int eat(sc_t *s, char c)
{
    if (peek(s) != c) return 0;
    s->p++;
    return 1;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

static char *put_utf8(char *w, unsigned cp)
{
    if (cp < 0x80) *w++ = cp;
    else if (cp < 0x800) { *w++ = 0xC0 | cp >> 6; *w++ = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { *w++ = 0xE0 | cp >> 12; *w++ = 0x80 | ((cp >> 6) & 0x3F); *w++ = 0x80 | (cp & 0x3F); }
    else { *w++ = 0xF0 | cp >> 18; *w++ = 0x80 | ((cp >> 12) & 0x3F); *w++ = 0x80 | ((cp >> 6) & 0x3F); *w++ = 0x80 | (cp & 0x3F); }
    return w;
}

// A JSON string at s->p, unescaped in place (the result is never longer).
static int scan_string(sc_t *s, const char **out, uint32_t *len)
{
    if (!eat(s, '"')) return 0;
    char *start = s->p, *w = s->p;
    while (s->p < s->end) {
        unsigned char c = *s->p++;
        if (c == '"') {
            *out = start;
            *len = w - start;
            return 1;
        }
        if (c < 0x20) return 0;
        if (c != '\\') { *w++ = c; continue; }
        if (s->p >= s->end) return 0;
        c = *s->p++;
        switch (c) {
        case '"': case '\\': case '/': *w++ = c; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (s->end - s->p < 4 || !hex4(s->p, &cp)) return 0;
            s->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u' &&
                hex4(s->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                s->p += 6;
            }
            w = put_utf8(w, cp);
            break;
        }
        default: return 0;
        }
    }
    return 0;
}

static int skip_value(sc_t *s, int depth)
{
    int c = peek(s);
    if (depth > MAX_DEPTH) return 0;
    if (c == '"') {
        const char *x;
        uint32_t n;
        return scan_string(s, &x, &n);
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        s->p++;
        if (eat(s, close)) return 1;
        do {
            if (c == '{') {
                const char *k;
                uint32_t n;
                if (!scan_string(s, &k, &n) || !eat(s, ':')) return 0;
            }
            if (!skip_value(s, depth + 1)) return 0;
        } while (eat(s, ','));
        return eat(s, close);
    }
    const char *start = s->p;
    if (s->end - s->p >= 4 && (!memcmp(s->p, "true", 4) || !memcmp(s->p, "null", 4))) { s->p += 4; return 1; }
    if (s->end - s->p >= 5 && !memcmp(s->p, "false", 5)) { s->p += 5; return 1; }
    while (s->p < s->end && strchr("-+0123456789.eE", *s->p)) s->p++;
    return s->p > start;
}

// A field value: strings stay in the buffer, anything else is parsed by cJSON.
static int scan_field(sc_t *s, jval_t *v)
{
    if (v->json) cJSON_Delete(v->json);  // duplicate key: the last one wins, like json.loads
    *v = (jval_t){.present = 1};
    if (peek(s) == '"') return scan_string(s, &v->s, &v->len);
    const char *end;
    v->json = cJSON_ParseWithLengthOpts(s->p, s->end - s->p, &end, 0);
    if (!v->json) return 0;
    s->p = (char *)end;
    return 1;
}

static int key_is(const char *k, uint32_t n, const char *want)
{
    return strlen(want) == n && !memcmp(k, want, n);
}

// --- dates: strptime("%Y-%m-%dT%H:%M:%S%z") and isoformat() -----------------

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int days_in_month(int y, int m)
{
    static const int D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : D[m - 1];
}

// 1 or 2 digits, largest valid value first (like strptime's regex alternatives)
static int num12(const char **p, const char *end, int lo, int hi, int *out)
{
    const char *s = *p;
    if (s < end && isdigit((unsigned char)s[0]) && s + 1 < end && isdigit((unsigned char)s[1])) {
        int v = (s[0] - '0') * 10 + (s[1] - '0');
        if (v >= lo && v <= hi) { *out = v; *p = s + 2; return 1; }
    }
    if (s < end && isdigit((unsigned char)s[0]) && s[0] - '0' >= lo && s[0] - '0' <= hi) {
        *out = s[0] - '0';
        *p = s + 1;
        return 1;
    }
    return 0;
}

static int two_digits(const char **p, const char *end, int *out)
{
    const char *s = *p;
    if (end - s < 2 || !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return 0;
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    *p = s + 2;
    return 1;
}

static int lit(const char **p, const char *end, char c)
{
    if (*p >= end || tolower((unsigned char)**p) != tolower((unsigned char)c)) return 0;
    (*p)++;
    return 1;
}

static int parse_date(const char *s, uint32_t len, int64_t *instant_us, int32_t *offset_s, int *year)
{
    const char *p = s, *end = s + len;
    int y = 0, mo, d, h, mi, sec, oh, om, os = 0;
    for (int i = 0; i < 4; i++) {
        if (p >= end || !isdigit((unsigned char)*p)) return 0;
        y = y * 10 + (*p++ - '0');
    }
    if (!lit(&p, end, '-') || !num12(&p, end, 1, 12, &mo) || !lit(&p, end, '-') || !num12(&p, end, 1, 31, &d) ||
        !lit(&p, end, 'T') || !num12(&p, end, 0, 23, &h) || !lit(&p, end, ':') || !num12(&p, end, 0, 59, &mi) ||
        !lit(&p, end, ':') || !num12(&p, end, 0, 61, &sec))
        return 0;
    if (y < 1 || d > days_in_month(y, mo) || sec > 59) return 0;  // datetime() would raise
    int sign = 0;
    if (p < end && (*p == 'Z' || *p == 'z')) {
        p++;
        oh = om = 0;
    } else {
        if (p >= end || (*p != '+' && *p != '-')) return 0;
        sign = *p++ == '-' ? -1 : 1;
        if (!two_digits(&p, end, &oh)) return 0;
        int colon = p < end && *p == ':';
        if (colon) p++;
        if (!two_digits(&p, end, &om) || om > 59) return 0;
        if (p < end && (colon ? *p == ':' : isdigit((unsigned char)*p))) {
            if (colon) p++;
            if (!two_digits(&p, end, &os) || os > 59) return 0;
        }
        if (oh >= 24) return 0;  // timezone() wants |offset| < 24 h
    }
    if (p != end) return 0;  // "unconverted data remains"
    int32_t off = (sign ? sign : 1) * (oh * 3600 + om * 60 + os);
    *offset_s = off;
    *year = y;
    *instant_us = ((days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec) - off) * US;
    return 1;
}

// datetime.isoformat() of the instant shown at `offset_s`
static void format_iso(int64_t instant_us, int32_t offset_s, char *buf, size_t size)
{
    int64_t local = instant_us + (int64_t)offset_s * US;
    int64_t secs = local >= 0 ? local / US : -((-local + US - 1) / US);
    int64_t us = local - secs * US;
    int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    int64_t rem = secs - days * 86400;
    int y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    int n = snprintf(buf, size, "%04d-%02u-%02uT%02d:%02d:%02d", y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60),
                     (int)(rem % 60));
    if (us) n += snprintf(buf + n, size - n, ".%06d", (int)us);
    int32_t a = offset_s < 0 ? -offset_s : offset_s;
    n += snprintf(buf + n, size - n, "%c%02d:%02d", offset_s < 0 ? '-' : '+', (int)(a / 3600), (int)(a / 60 % 60));
    if (a % 60) snprintf(buf + n, size - n, ":%02d", (int)(a % 60));
}

// --- parsing the reply --------------------------------------------------------

static int cmp_events(const void *a, const void *b)
{
    const event_t *x = a, *y = b;
    if (x->t_us != y->t_us) return x->t_us < y->t_us ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;  // stable, like list.sort()
}

static void free_fields(event_t *e)
{
    cJSON_Delete(e->level.json);
    cJSON_Delete(e->module.json);
    cJSON_Delete(e->message.json);
}

void eventlog_free(event_log_t *log)
{
    if (!log) return;
    for (size_t i = 0; i < log->count; i++) free_fields(&log->events[i]);
    free(log->events);
    free(log);
}

// entries of `[ {..., "log": [ {date, log, module, param, ...}, ... ]}, ... ]`
static int scan_entries(sc_t *s, event_log_t *log, int64_t boot_us, pj_err_t *err)
{
    for (size_t i = 0; i < log->count; i++) free_fields(&log->events[i]);  // an earlier "log" key
    log->count = 0;
    if (!eat(s, '[')) {
        pj_fail(err, "the router's log is not a list");
        return 0;
    }
    if (eat(s, ']')) return 1;
    size_t cap = log->count;
    do {
        if (peek(s) != '{') {
            pj_fail(err, "log entry %u is not an object", (unsigned)log->count);
            return 0;
        }
        s->p++;
        event_t e = {.index = (uint32_t)log->count};
        jval_t date = {0};
        if (!eat(s, '}')) {
            do {
                const char *k;
                uint32_t n;
                if (!scan_string(s, &k, &n) || !eat(s, ':')) goto bad;
                jval_t *slot = key_is(k, n, "date") ? &date : key_is(k, n, "log") ? &e.level
                             : key_is(k, n, "module") ? &e.module : key_is(k, n, "param") ? &e.message : NULL;
                if (slot ? !scan_field(s, slot) : !skip_value(s, 1)) goto bad;
            } while (eat(s, ','));
            if (!eat(s, '}')) goto bad;
        }
        int year;
        if (!date.present) {
            pj_fail(err, "log entry %u has no 'date'", (unsigned)e.index);
        } else if (!date.s) {
            pj_fail(err, "log entry %u: strptime() argument must be str", (unsigned)e.index);
        } else if (!parse_date(date.s, date.len, &e.t_us, &e.offset_s, &year)) {
            pj_fail(err, "time data '%.*s' does not match format '%%Y-%%m-%%dT%%H:%%M:%%S%%z'", (int)(date.len > 40 ? 40 : date.len), date.s);
        } else if (year < 2020) {
            e.corrected = 1;
            e.t_us = boot_us + (e.t_us - EPOCH_2013_US);
        }
        cJSON_Delete(date.json);
        if (err->failed) {
            free_fields(&e);
            return 0;
        }
        if (log->count == cap) {
            cap = cap ? cap * 2 : 1024;
            event_t *grown = realloc(log->events, cap * sizeof(event_t));
            if (!grown) {
                free_fields(&e);
                pj_fail(err, "out of memory for %u log entries", (unsigned)cap);
                return 0;
            }
            log->events = grown;
        }
        log->events[log->count++] = e;
        continue;
    bad:
        free_fields(&e);
        cJSON_Delete(date.json);
        pj_fail(err, "invalid JSON in the router's log near entry %u", (unsigned)e.index);
        return 0;
    } while (eat(s, ','));
    if (!eat(s, ']')) {
        pj_fail(err, "invalid JSON at the end of the router's log");
        return 0;
    }
    return 1;
}

event_log_t *eventlog_parse(char *raw, size_t len, int64_t boot_us, pj_err_t *err)
{
    event_log_t *log = calloc(1, sizeof(*log));
    sc_t s = {raw, raw + len};
    int have_log = 0;
    if (!log) return NULL;
    if (!eat(&s, '[')) goto bad;
    if (peek(&s) == ']') {
        pj_fail(err, "list index out of range");  // d[0]
        goto fail;
    }
    if (peek(&s) != '{') {
        pj_fail(err, "the router's reply is not a list of objects");
        goto fail;
    }
    s.p++;
    if (!eat(&s, '}')) {
        do {
            const char *k;
            uint32_t n;
            if (!scan_string(&s, &k, &n) || !eat(&s, ':')) goto bad;
            if (key_is(k, n, "log")) {
                if (!scan_entries(&s, log, boot_us, err)) goto fail;
                have_log = 1;
            } else if (!skip_value(&s, 1)) {
                goto bad;
            }
        } while (eat(&s, ','));
        if (!eat(&s, '}')) goto bad;
    }
    while (eat(&s, ','))
        if (!skip_value(&s, 1)) goto bad;
    if (!eat(&s, ']') || peek(&s) != -1) goto bad;
    if (!have_log) {
        pj_fail(err, "missing field 'log'");
        goto fail;
    }
    qsort(log->events, log->count, sizeof(event_t), cmp_events);
    return log;
bad:
    pj_fail(err, "invalid JSON from the router's log");
fail:
    eventlog_free(log);
    return NULL;
}

// --- classify(): EVENT_PATTERNS as hand-written matchers ---------------------
// Same order and the same re.search() semantics (first matching kind wins;
// `.` stops at a newline, `[^x]` does not; re.I where the pattern has it).

enum {
    K_WIFI_CONNECT, K_WIFI_DISCONNECT, K_WIFI_AUTH_FAILURE, K_BOOT_REASON, K_POWER_UP, K_WAN_DOWN, K_WAN_UP,
    K_WAN_DHCP_STOPPED, K_WAN_DHCP_STARTED, K_TR069_FAILED, K_TR069_SESSION, K_GUI_LOGIN, K_GUI_LOGOUT,
    K_TR069_CLOSED, K_DNS_ACTIVE, K_DHCP_SERVER_ACTIVE, K_GUI_LOGIN_FAILED, K_LAN_PORT_UP, K_LAN_PORT_DOWN,
    K_OTHER, K_COUNT
};
static const char *const KIND_NAMES[K_COUNT] = {
    "wifi_connect", "wifi_disconnect", "wifi_auth_failure", "boot_reason", "power_up", "wan_down", "wan_up",
    "wan_dhcp_stopped", "wan_dhcp_started", "tr069_failed", "tr069_session", "gui_login", "gui_logout",
    "tr069_closed", "dns_active", "dhcp_server_active", "gui_login_failed", "lan_port_up", "lan_port_down",
    "other",
};

typedef struct {
    const char *p;
    size_t n;
} span_t;

typedef struct {
    int kind;
    char mac[18];  // lowercase, "" when the pattern has no mac group
    span_t ssid, ip;
} match_t;

typedef struct {
    const char *s;
    size_t n;
} text_t;

static long find(text_t t, size_t from, const char *needle, int icase)
{
    size_t k = strlen(needle);
    for (size_t i = from; i + k <= t.n; i++) {
        size_t j = 0;
        if (icase) while (j < k && tolower((unsigned char)t.s[i + j]) == tolower((unsigned char)needle[j])) j++;
        else while (j < k && t.s[i + j] == needle[j]) j++;
        if (j == k) return (long)i;
    }
    return -1;
}

static int at(text_t t, size_t i, const char *lit)
{
    size_t k = strlen(lit);
    return i + k <= t.n && !memcmp(t.s + i, lit, k);
}

// [0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}
static int mac_at(text_t t, size_t i, char out[18])
{
    if (i + 17 > t.n) return 0;
    for (int j = 0; j < 17; j++) {
        char c = t.s[i + j];
        if (j % 3 == 2 ? c != ':' : !isxdigit((unsigned char)c)) return 0;
        out[j] = tolower((unsigned char)c);
    }
    out[17] = '\0';
    return 1;
}

// `\(.*SSID\[(?P<ssid>[^\]]+)\]\)` starting right after the '(' at `i`
static int ssid_tail(text_t t, size_t i, span_t *ssid)
{
    size_t line_end = i;
    while (line_end < t.n && t.s[line_end] != '\n') line_end++;  // `.*` stops at a newline
    for (size_t q = line_end + 1; q-- > i;) {                      // greedy: the last "SSID[" that works
        if (!at(t, q, "SSID[")) continue;
        size_t r = q + 5;
        while (r < t.n && t.s[r] != ']') r++;
        if (r > q + 5 && r + 1 < t.n && t.s[r + 1] == ')') {
            *ssid = (span_t){t.s + q + 5, r - q - 5};
            return 1;
        }
    }
    return 0;
}

static int m_wifi(text_t t, match_t *m, const char *prefix, const char *middle)
{
    size_t pl = strlen(prefix);
    for (long i = find(t, 0, prefix, 0); i >= 0; i = find(t, i + 1, prefix, 0)) {
        size_t p = i + pl;
        if (mac_at(t, p, m->mac) && at(t, p + 17, middle) && ssid_tail(t, p + 17 + strlen(middle), &m->ssid)) return 1;
    }
    return 0;
}

static int m_auth_failure(text_t t, match_t *m)
{
    static const char P[] = "A device failed to connect to SSID (";
    for (long i = find(t, 0, P, 0); i >= 0; i = find(t, i + 1, P, 0)) {
        size_t p = i + sizeof(P) - 1;
        if (mac_at(t, p, m->mac) && at(t, p + 17, ") because it provided incorrect login")) return 1;
    }
    return 0;
}

// `Current boot was caused by (?P<reason>[^=]+?)\s*=`
static int m_boot_reason(text_t t)
{
    static const char P[] = "Current boot was caused by ";
    for (long i = find(t, 0, P, 0); i >= 0; i = find(t, i + 1, P, 0)) {
        size_t p = i + sizeof(P) - 1, e = p;
        while (e < t.n && t.s[e] != '=') e++;
        if (e < t.n && e > p) return 1;
    }
    return 0;
}

// `<prefix>(\d+)<suffix>`
static int m_digits(text_t t, const char *prefix, const char *suffix)
{
    size_t pl = strlen(prefix);
    for (long i = find(t, 0, prefix, 0); i >= 0; i = find(t, i + 1, prefix, 0)) {
        size_t p = i + pl, e = p;
        while (e < t.n && isdigit((unsigned char)t.s[e])) e++;
        if (e > p && at(t, e, suffix)) return 1;
    }
    return 0;
}

// `TR-069 connectivity to \((?P<host>[^)]+)\) <suffix>`
static int m_tr069(text_t t, const char *suffix)
{
    static const char P[] = "TR-069 connectivity to (";
    for (long i = find(t, 0, P, 0); i >= 0; i = find(t, i + 1, P, 0)) {
        size_t p = i + sizeof(P) - 1, e = p;
        while (e < t.n && t.s[e] != ')') e++;
        if (e > p && e < t.n && at(t, e + 1, suffix)) return 1;
    }
    return 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// `<prefix>(?P<user>\S+) from (?P<ip>\S+)`
static int m_gui(text_t t, const char *prefix, match_t *m)
{
    size_t pl = strlen(prefix);
    for (long i = find(t, 0, prefix, 0); i >= 0; i = find(t, i + 1, prefix, 0)) {
        size_t p = i + pl, e = p;
        while (e < t.n && !is_space(t.s[e])) e++;
        if (e == p || !at(t, e, " from ")) continue;
        size_t ip = e + 6, ie = ip;
        while (ie < t.n && !is_space(t.s[ie])) ie++;
        if (ie > ip) {
            m->ip = (span_t){t.s + ip, ie - ip};
            return 1;
        }
    }
    return 0;
}

// re.I `GUI login .*(fail|incorrect|denied)`
static int m_gui_login_failed(text_t t)
{
    for (long i = find(t, 0, "GUI login ", 1); i >= 0; i = find(t, i + 1, "GUI login ", 1)) {
        size_t p = i + 10, e = p;
        while (e < t.n && t.s[e] != '\n') e++;
        text_t rest = {t.s + p, e - p};
        if (find(rest, 0, "fail", 1) >= 0 || find(rest, 0, "incorrect", 1) >= 0 || find(rest, 0, "denied", 1) >= 0)
            return 1;
    }
    return 0;
}

static int is_word(unsigned char c)
{
    return isalnum(c) || c == '_' || c >= 0x80;  // \w (non-ASCII letters count in Python too)
}

// `An Ethernet port is now connected \((\d+)/(\d+)/(\w+)\)`
static int m_lan_port_up(text_t t)
{
    static const char P[] = "An Ethernet port is now connected (";
    for (long i = find(t, 0, P, 0); i >= 0; i = find(t, i + 1, P, 0)) {
        size_t p = i + sizeof(P) - 1, e = p;
        int ok = 1;
        for (int part = 0; part < 2 && ok; part++) {
            size_t s = e;
            while (e < t.n && isdigit((unsigned char)t.s[e])) e++;
            ok = e > s && e < t.n && t.s[e++] == '/';
        }
        if (!ok) continue;
        size_t s = e;
        while (e < t.n && is_word(t.s[e])) e++;
        if (e > s && e < t.n && t.s[e] == ')') return 1;
    }
    return 0;
}

static void classify_text(text_t t, match_t *m)
{
    *m = (match_t){0};
    int k = K_OTHER;
    if (m_wifi(t, m, "A WiFi device <", "> has successfully connected to SSID (")) k = K_WIFI_CONNECT;
    else if (m_wifi(t, m, "Device <", "> was disconnected on SSID (")) k = K_WIFI_DISCONNECT;
    else if (m_auth_failure(t, m)) k = K_WIFI_AUTH_FAILURE;
    else if (m_boot_reason(t)) k = K_BOOT_REASON;
    else if (find(t, 0, "The Modem has successfully powered up", 0) >= 0) k = K_POWER_UP;
    else if (find(t, 0, "WAN Ethernet connectivity has been disconnected", 1) >= 0) k = K_WAN_DOWN;
    else if (find(t, 0, "WAN Ethernet connectivity has been established", 1) >= 0) k = K_WAN_UP;
    else if (m_digits(t, "WAN DHCP client (", ") stopped")) k = K_WAN_DHCP_STOPPED;
    else if (m_digits(t, "WAN DHCP client (", ") started")) k = K_WAN_DHCP_STARTED;
    else if (m_tr069(t, " has failed")) k = K_TR069_FAILED;
    else if (m_tr069(t, " has been initiated")) k = K_TR069_SESSION;
    else if (m_gui(t, "GUI login was successful for user ", m)) k = K_GUI_LOGIN;
    else if (m_gui(t, "GUI logout was successful for user ", m)) k = K_GUI_LOGOUT;
    else if (m_tr069(t, " has been closed")) k = K_TR069_CLOSED;
    else if (find(t, 0, "DNS name resolution is now active", 0) >= 0) k = K_DNS_ACTIVE;
    else if (find(t, 0, "The LAN DHCP Server is active", 0) >= 0) k = K_DHCP_SERVER_ACTIVE;
    else if (m_gui_login_failed(t)) k = K_GUI_LOGIN_FAILED;
    else if (m_lan_port_up(t)) k = K_LAN_PORT_UP;
    else if (find(t, 0, "An Ethernet port is now disconnected", 1) >= 0) k = K_LAN_PORT_DOWN;
    // only the kinds whose pattern has the group keep it
    if (k != K_WIFI_CONNECT && k != K_WIFI_DISCONNECT && k != K_WIFI_AUTH_FAILURE) m->mac[0] = '\0';
    if (k != K_WIFI_CONNECT && k != K_WIFI_DISCONNECT) m->ssid = (span_t){0};
    if (k != K_GUI_LOGIN && k != K_GUI_LOGOUT) m->ip = (span_t){0};
    m->kind = k;
}

// classify(event): the message must be a str (re.search raises otherwise)
static int classify(const event_t *e, match_t *m, pj_err_t *err)
{
    if (!e->message.s) {
        const cJSON *j = e->message.json;
        pj_fail(err, "expected string or bytes-like object, got '%s'",
                !j || cJSON_IsNull(j) ? "NoneType" : cJSON_IsBool(j) ? "bool" : cJSON_IsNumber(j) ? "number"
                : cJSON_IsArray(j) ? "list" : "dict");
        return 0;
    }
    classify_text((text_t){e->message.s, e->message.len}, m);
    return 1;
}

// --- Names ---------------------------------------------------------------------

typedef struct {
    char *key;           // lowercase
    const cJSON *value;  // NULL = None
} nentry_t;

typedef struct {
    nentry_t *items;
    size_t count, cap;
} ndict_t;

struct names {
    ndict_t by_mac, nodes;
    cJSON *gateway_literal;  // "gateway" when the gateway has no hostname
};

static char *lower_dup(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i <= n; i++) out[i] = tolower((unsigned char)s[i]);
    return out;
}

static nentry_t *nd_find(const ndict_t *d, const char *key)
{
    for (size_t i = 0; i < d->count; i++)
        if (!strcmp(d->items[i].key, key)) return &d->items[i];
    return NULL;
}

// d[key.lower()] = value (keeps the position of an existing key, like a dict)
static int nd_set(ndict_t *d, const char *key, const cJSON *value)
{
    char *k = lower_dup(key);
    if (!k) return 0;
    nentry_t *e = nd_find(d, k);
    if (e) {
        free(k);
        e->value = value;
        return 1;
    }
    if (d->count == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        nentry_t *grown = realloc(d->items, cap * sizeof(nentry_t));
        if (!grown) { free(k); return 0; }
        d->items = grown;
        d->cap = cap;
    }
    d->items[d->count++] = (nentry_t){k, value};
    return 1;
}

static void nd_free(ndict_t *d)
{
    for (size_t i = 0; i < d->count; i++) free(d->items[i].key);
    free(d->items);
}

void names_free(names_t *n)
{
    if (!n) return;
    nd_free(&n->by_mac);
    nd_free(&n->nodes);
    cJSON_Delete(n->gateway_literal);
    free(n);
}

// {x[key].lower(): x[value] for x in list if x.get(key)}
static void add_all(ndict_t *d, const cJSON *list, const char *key, const char *value, pj_err_t *err)
{
    const cJSON *x;
    cJSON_ArrayForEach(x, list) {
        const cJSON *k = pj_get(x, key);
        if (!pj_truthy(k)) continue;
        if (!cJSON_IsString(k)) { pj_fail(err, "'%s' is not a string", key); return; }
        if (!nd_set(d, k->valuestring, pj_get(x, value))) { pj_fail(err, "out of memory"); return; }
    }
}

names_t *names_build(const cJSON *hosts, const cJSON *extenders, const cJSON *gateway, const cJSON *devices,
                     pj_err_t *err)
{
    names_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    add_all(&n->by_mac, hosts, "mac", "name", err);
    add_all(&n->by_mac, devices, "mac", "name", err);
    add_all(&n->nodes, extenders, "device_id", "hostname", err);
    const cJSON *gw_id = pj_get(gateway, "device_id"), *gw_name = pj_get(gateway, "hostname");
    if (pj_truthy(gw_id)) {
        if (!cJSON_IsString(gw_id)) {
            pj_fail(err, "gateway device_id is not a string");
        } else {
            if (!pj_truthy(gw_name)) gw_name = n->gateway_literal = cJSON_CreateString("gateway");
            nd_set(&n->nodes, gw_id->valuestring, gw_name);
        }
    }
    if (err->failed) {
        names_free(n);
        return NULL;
    }
    return n;
}

// int(s, 16)
static int py_hex(const char *s, long *out)
{
    while (is_space(*s)) s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) s += 2;
    long v = 0;
    int digits = 0;
    for (; isxdigit((unsigned char)*s) || (*s == '_' && digits && isxdigit((unsigned char)s[1])); s++) {
        if (*s == '_') continue;
        v = v * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
        digits++;
    }
    while (is_space(*s)) s++;
    *out = neg ? -v : v;
    return digits && !*s;
}

// Names.mesh_node_for(): mesh nodes use 32 consecutive MACs from their deviceId
static const cJSON *mesh_node_for(const names_t *n, const char *mac, int *is_node, pj_err_t *err)
{
    const nentry_t *exact = nd_find(&n->nodes, mac);
    *is_node = 0;
    if (exact) {
        *is_node = pj_truthy(exact->value);
        return exact->value;
    }
    const nentry_t *best = NULL;
    long best_diff = 0, mac_tail;
    if (strlen(mac) < 14 || !py_hex(mac + (strlen(mac) > 15 ? 15 : strlen(mac)), &mac_tail)) mac_tail = -1;
    for (size_t i = 0; i < n->nodes.count; i++) {
        const char *id = n->nodes.items[i].key;
        if (strlen(id) < 14 || strncmp(id, mac, 14)) continue;
        long id_tail;
        if (mac_tail < 0 || !py_hex(id + (strlen(id) > 15 ? 15 : strlen(id)), &id_tail)) {
            pj_fail(err, "invalid literal for int() with base 16 in a MAC");
            return NULL;
        }
        long diff = mac_tail - id_tail;
        if (diff >= 0 && diff < 0x20 && (!best || diff < best_diff)) {
            best = &n->nodes.items[i];
            best_diff = diff;
        }
    }
    if (!best) return NULL;
    *is_node = pj_truthy(best->value);
    return best->value;
}

// Names.resolve(mac)
static cJSON *resolve(const names_t *n, const char *mac, pj_err_t *err)
{
    int is_node;
    const cJSON *node = mesh_node_for(n, mac, &is_node, err);
    const nentry_t *client = nd_find(&n->by_mac, mac);
    long first;
    char head[3] = {mac[0], mac[0] ? mac[1] : 0, 0};
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObjectCS(out, "mac", cJSON_CreateString(mac));
    cJSON_AddItemToObjectCS(out, "name", pj_dup(is_node ? node : client ? client->value : NULL));
    cJSON_AddItemToObjectCS(out, "kind", cJSON_CreateStringReference(is_node ? "mesh_node" : client ? "client" : "unknown"));
    cJSON_AddItemToObjectCS(out, "private_mac", cJSON_CreateBool(py_hex(head, &first) && (first & 0x02)));
    return out;
}

// --- event_log / event_summary ----------------------------------------------

static cJSON *jval_json(const jval_t *v)
{
    if (v->s) {
        char small[256], *buf = v->len < sizeof(small) ? small : malloc(v->len + 1);
        if (!buf) return cJSON_CreateNull();
        memcpy(buf, v->s, v->len);
        buf[v->len] = '\0';
        cJSON *out = cJSON_CreateString(buf);
        if (buf != small) free(buf);
        return out;
    }
    return v->json ? cJSON_Duplicate(v->json, 1) : cJSON_CreateNull();
}

// str(value) as Python would print it inside an f-string
static void jval_pystr(const jval_t *v, char *buf, size_t size)
{
    if (v->s) snprintf(buf, size, "%.*s", (int)v->len, v->s);
    else if (!v->json || cJSON_IsNull(v->json)) snprintf(buf, size, "None");
    else if (cJSON_IsBool(v->json)) snprintf(buf, size, cJSON_IsTrue(v->json) ? "True" : "False");
    else if (cJSON_IsNumber(v->json)) pj_key_str(v->json, buf, size);
    else {
        char *s = cJSON_PrintUnformatted(v->json);
        snprintf(buf, size, "%s", s ? s : "?");
        cJSON_free(s);
    }
}

static cJSON *event_json(const event_t *e)
{
    char when[48];
    format_iso(e->t_us, e->offset_s, when, sizeof(when));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObjectCS(o, "time", cJSON_CreateString(when));
    cJSON_AddItemToObjectCS(o, "level", jval_json(&e->level));
    cJSON_AddItemToObjectCS(o, "module", jval_json(&e->module));
    cJSON_AddItemToObjectCS(o, "message", jval_json(&e->message));
    cJSON_AddItemToObjectCS(o, "clock_corrected", cJSON_CreateBool(e->corrected));
    return o;
}

// events_in_window(): indexes of events at or after now - hours
static size_t *window(const event_log_t *log, int64_t now_us, double hours, size_t *count, pj_err_t *err)
{
    if (!(fabs(hours) < 1.7e7)) {  // timedelta/datetime would overflow
        pj_fail(err, "date value out of range");
        return NULL;
    }
    int64_t start = now_us - llround(hours * 3600.0 * 1e6);
    size_t *idx = malloc((log->count ? log->count : 1) * sizeof(size_t)), n = 0;
    if (!idx) { pj_fail(err, "out of memory"); return NULL; }
    for (size_t i = 0; i < log->count; i++)
        if (log->events[i].t_us >= start) idx[n++] = i;
    *count = n;
    return idx;
}

// a.lower() == b.lower() / a.lower().startswith(b) / b in a.lower(), ASCII case folding
static int ieq(const jval_t *v, const char *s)
{
    size_t n = strlen(s);
    if (v->len != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)v->s[i]) != tolower((unsigned char)s[i])) return 0;
    return 1;
}

static int istarts(const jval_t *v, const char *s, size_t n)
{
    if (v->len < n) return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)v->s[i]) != tolower((unsigned char)s[i])) return 0;
    return 1;
}

static int icontains(const char *hay, size_t hn, const char *needle)
{
    return find((text_t){hay, hn}, 0, needle, 1) >= 0;
}

static int need_str(const jval_t *v, const char *field, pj_err_t *err)
{
    if (v->s) return 1;
    pj_fail(err, "'%s' of an event is not a string (no attribute 'lower')", field);
    return 0;
}

cJSON *eventlog_query(const event_log_t *log, const names_t *names, int64_t now_us, double hours,
                      const char *module, const char *level, const char *device_needle, const char *contains,
                      long long limit, pj_err_t *err)
{
    size_t n, kept;
    size_t *sel = window(log, now_us, hours, &n, err);
    if (!sel) return NULL;
    // filters in the Python order, each over what the previous one kept
    if (module && *module) {
        kept = 0;
        for (size_t i = 0; i < n && !err->failed; i++)
            if (need_str(&log->events[sel[i]].module, "module", err) && ieq(&log->events[sel[i]].module, module))
                sel[kept++] = sel[i];
        n = kept;
    }
    if (level && *level && !err->failed) {
        size_t three = 0;  // level.lower()[:3]
        while (three < 3 && level[three]) three++;
        kept = 0;
        for (size_t i = 0; i < n && !err->failed; i++)
            if (need_str(&log->events[sel[i]].level, "level", err) && istarts(&log->events[sel[i]].level, level, three))
                sel[kept++] = sel[i];
        n = kept;
    }
    if (contains && *contains && !err->failed) {
        kept = 0;
        for (size_t i = 0; i < n && !err->failed; i++) {
            const jval_t *m = &log->events[sel[i]].message;
            if (need_str(m, "message", err) && icontains(m->s, m->len, contains)) sel[kept++] = sel[i];
        }
        n = kept;
    }
    if (device_needle && *device_needle && !err->failed) {
        kept = 0;
        for (size_t i = 0; i < n && !err->failed; i++) {
            match_t m;
            if (!classify(&log->events[sel[i]], &m, err) || !m.mac[0]) continue;
            int hit = strstr(m.mac, device_needle) != NULL;
            if (!hit) {
                cJSON *who = resolve(names, m.mac, err);
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(who, "name");
                hit = cJSON_IsString(name) && icontains(name->valuestring, strlen(name->valuestring), device_needle);
                cJSON_Delete(who);
            }
            if (hit) sel[kept++] = sel[i];
        }
        n = kept;
    }
    if (err->failed) {
        free(sel);
        return NULL;
    }

    // selected[-limit:] if limit else selected, newest first
    size_t from = 0;
    if (limit) {
        long long start = -limit;
        if (start < 0) start += (long long)n;
        from = start < 0 ? 0 : (size_t)start > n ? n : (size_t)start;
    }
    cJSON *events = cJSON_CreateArray();
    for (size_t i = n; i-- > from && !err->failed;) {
        const event_t *e = &log->events[sel[i]];
        match_t m;
        if (!classify(e, &m, err)) break;
        cJSON *entry = event_json(e);
        cJSON_AddItemToObjectCS(entry, "kind", cJSON_CreateStringReference(KIND_NAMES[m.kind]));
        if (m.mac[0]) cJSON_AddItemToObjectCS(entry, "device", resolve(names, m.mac, err));
        cJSON_AddItemToArray(events, entry);
    }
    free(sel);
    if (err->failed) {
        cJSON_Delete(events);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "matched", (double)n);
    cJSON_AddNumberToObject(out, "returned", cJSON_GetArraySize(events));
    cJSON_AddItemToObject(out, "events", events);
    return out;
}

// Counter in insertion order, for most_common()
typedef struct {
    char *key;
    long count;
    long order;
} counted_t;

typedef struct {
    counted_t *items;
    size_t count, cap;
} counter_t;

static void counter_add(counter_t *c, const char *key, pj_err_t *err)
{
    for (size_t i = 0; i < c->count; i++)
        if (!strcmp(c->items[i].key, key)) { c->items[i].count++; return; }
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        counted_t *grown = realloc(c->items, cap * sizeof(counted_t));
        if (!grown) { pj_fail(err, "out of memory"); return; }
        c->items = grown;
        c->cap = cap;
    }
    char *k = malloc(strlen(key) + 1);
    if (!k) { pj_fail(err, "out of memory"); return; }
    strcpy(k, key);
    c->items[c->count] = (counted_t){k, 1, (long)c->count};
    c->count++;
}

static int by_count(const void *a, const void *b)
{
    const counted_t *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static void counter_free(counter_t *c)
{
    for (size_t i = 0; i < c->count; i++) free(c->items[i].key);
    free(c->items);
}

typedef struct {
    char mac[18];
    long connects, disconnects, auth_failures, order;
    char **ssids;
    size_t n_ssids;
} dev_t;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int by_activity(const void *a, const void *b)
{
    const dev_t *x = a, *y = b;
    long ax = x->connects + x->disconnects + x->auth_failures, ay = y->connects + y->disconnects + y->auth_failures;
    if (ax != ay) return ax > ay ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

cJSON *eventlog_summary(const event_log_t *log, const names_t *names, int64_t now_us, const cJSON *hours,
                        const char *own_ip, pj_err_t *err)
{
    size_t n;
    size_t *sel = window(log, now_us, cJSON_IsNumber(hours) ? hours->valuedouble : 25, &n, err);
    if (!sel) return NULL;
    counter_t kinds = {0}, logins = {0}, other = {0};
    dev_t *devs = NULL;
    size_t n_devs = 0, cap_devs = 0;
    char text[512];

    for (size_t i = 0; i < n && !err->failed; i++) {
        const event_t *e = &log->events[sel[i]];
        match_t m;
        if (!classify(e, &m, err)) break;
        counter_add(&kinds, KIND_NAMES[m.kind], err);
        if (m.mac[0]) {
            dev_t *d = NULL;
            for (size_t j = 0; j < n_devs && !d; j++)
                if (!strcmp(devs[j].mac, m.mac)) d = &devs[j];
            if (!d) {
                if (n_devs == cap_devs) {
                    cap_devs = cap_devs ? cap_devs * 2 : 32;
                    dev_t *grown = realloc(devs, cap_devs * sizeof(dev_t));
                    if (!grown) { pj_fail(err, "out of memory"); break; }
                    devs = grown;
                }
                d = &devs[n_devs];
                *d = (dev_t){.order = (long)n_devs};
                memcpy(d->mac, m.mac, sizeof(d->mac));
                n_devs++;
            }
            d->connects += m.kind == K_WIFI_CONNECT;
            d->disconnects += m.kind == K_WIFI_DISCONNECT;
            d->auth_failures += m.kind == K_WIFI_AUTH_FAILURE;
            if (m.ssid.n) {
                snprintf(text, sizeof(text), "%.*s", (int)m.ssid.n, m.ssid.p);
                int seen = 0;
                for (size_t j = 0; j < d->n_ssids && !seen; j++) seen = !strcmp(d->ssids[j], text);
                if (!seen) {
                    char **grown = realloc(d->ssids, (d->n_ssids + 1) * sizeof(char *));
                    char *copy = malloc(strlen(text) + 1);
                    if (!grown || !copy) { free(copy); if (grown) d->ssids = grown; pj_fail(err, "out of memory"); break; }
                    strcpy(copy, text);
                    d->ssids = grown;
                    d->ssids[d->n_ssids++] = copy;
                }
            }
        }
        if (m.kind == K_GUI_LOGIN) {
            int own = own_ip && strlen(own_ip) == m.ip.n && !memcmp(own_ip, m.ip.p, m.ip.n);
            snprintf(text, sizeof(text), "%.*s%s", (int)m.ip.n, m.ip.p, own ? " (this MCP server)" : "");
            counter_add(&logins, text, err);
        }
        if (m.kind == K_OTHER) {
            char mod[128];
            jval_pystr(&e->module, mod, sizeof(mod));
            snprintf(text, sizeof(text), "%s: %.*s", mod, (int)e->message.len, e->message.s);
            counter_add(&other, text, err);
        }
    }

    cJSON *out = NULL;
    if (!err->failed) {
        out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "window_hours", hours ? cJSON_Duplicate(hours, 1) : cJSON_CreateNumber(25));
        cJSON_AddNumberToObject(out, "total_events", (double)n);
        char when[48];
        if (n) {
            format_iso(log->events[sel[0]].t_us, log->events[sel[0]].offset_s, when, sizeof(when));
            cJSON_AddStringToObject(out, "first_event", when);
            format_iso(log->events[sel[n - 1]].t_us, log->events[sel[n - 1]].offset_s, when, sizeof(when));
            cJSON_AddStringToObject(out, "last_event", when);
        } else {
            cJSON_AddNullToObject(out, "first_event");
            cJSON_AddNullToObject(out, "last_event");
        }
        qsort(kinds.items, kinds.count, sizeof(counted_t), by_count);
        cJSON *counts = cJSON_AddObjectToObject(out, "event_counts");
        for (size_t i = 0; i < kinds.count; i++) cJSON_AddNumberToObject(counts, kinds.items[i].key, kinds.items[i].count);

        qsort(devs, n_devs, sizeof(dev_t), by_activity);
        cJSON *wifi = cJSON_AddArrayToObject(out, "wifi_devices");
        for (size_t i = 0; i < n_devs; i++) {
            cJSON *d = resolve(names, devs[i].mac, err);
            cJSON_AddNumberToObject(d, "connects", devs[i].connects);
            cJSON_AddNumberToObject(d, "disconnects", devs[i].disconnects);
            cJSON_AddNumberToObject(d, "auth_failures", devs[i].auth_failures);
            qsort(devs[i].ssids, devs[i].n_ssids, sizeof(char *), cmp_str);
            cJSON *ss = cJSON_AddArrayToObject(d, "ssids");
            for (size_t j = 0; j < devs[i].n_ssids; j++) cJSON_AddItemToArray(ss, cJSON_CreateString(devs[i].ssids[j]));
            cJSON_AddItemToArray(wifi, d);
        }
        cJSON *by_source = cJSON_AddObjectToObject(out, "gui_logins_by_source");  // dict(): insertion order
        for (size_t i = 0; i < logins.count; i++) cJSON_AddNumberToObject(by_source, logins.items[i].key, logins.items[i].count);
        qsort(other.items, other.count, sizeof(counted_t), by_count);
        cJSON *others = cJSON_AddArrayToObject(out, "other_events");
        for (size_t i = 0; i < other.count && i < 30; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "event", other.items[i].key);
            cJSON_AddNumberToObject(o, "count", other.items[i].count);
            cJSON_AddItemToArray(others, o);
        }
        if (err->failed) {
            cJSON_Delete(out);
            out = NULL;
        }
    }
    for (size_t i = 0; i < n_devs; i++) {
        for (size_t j = 0; j < devs[i].n_ssids; j++) free(devs[i].ssids[j]);
        free(devs[i].ssids);
    }
    free(devs);
    counter_free(&kinds);
    counter_free(&logins);
    counter_free(&other);
    free(sel);
    return out;
}
