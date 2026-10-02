#include "pyjson.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void pj_fail(pj_err_t *err, const char *fmt, ...)
{
    if (!err || err->failed) return;
    err->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, err->size, fmt, ap);
    va_end(ap);
}

int pj_truthy(const cJSON *v)
{
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != '\0';
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsArray(v) || cJSON_IsObject(v)) return v->child != NULL;
    return 1;
}

const cJSON *pj_or(const cJSON *a, const cJSON *b)
{
    return pj_truthy(a) ? a : b;
}

const cJSON *pj_get(const cJSON *obj, const char *key)
{
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

const cJSON *pj_req(const cJSON *obj, const char *key, pj_err_t *err)
{
    const cJSON *v = pj_get(obj, key);
    if (!v) pj_fail(err, cJSON_IsObject(obj) ? "missing field '%s'" : "no object holding '%s'", key);
    return v;
}

const cJSON *pj_item(const cJSON *arr, int index, pj_err_t *err)
{
    const cJSON *v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, index) : NULL;
    if (!v) pj_fail(err, "expected a list with an item %d", index);
    return v;
}

cJSON *pj_dup(const cJSON *v)
{
    return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
}

int pj_same_key(const cJSON *a, const cJSON *b)
{
    int a_none = !a || cJSON_IsNull(a), b_none = !b || cJSON_IsNull(b);
    if (a_none || b_none) return a_none && b_none;
    if (cJSON_IsString(a) && cJSON_IsString(b)) return strcmp(a->valuestring, b->valuestring) == 0;
    if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) return a->valuedouble == b->valuedouble;
    if (cJSON_IsBool(a) && cJSON_IsBool(b)) return cJSON_IsTrue(a) == cJSON_IsTrue(b);
    return 0;
}

int pj_is_str(const cJSON *v, const char *s)
{
    return cJSON_IsString(v) && strcmp(v->valuestring, s) == 0;
}

cJSON *pj_int(const cJSON *v, pj_err_t *err)
{
    if (cJSON_IsBool(v)) return cJSON_CreateNumber(cJSON_IsTrue(v));
    if (cJSON_IsNumber(v)) return cJSON_CreateNumber(trunc(v->valuedouble));
    if (cJSON_IsString(v)) {
        // int("  -1_000 ") == -1000: whitespace around, a sign, '_' only between digits
        const char *p = v->valuestring;
        while (isspace((unsigned char)*p)) p++;
        int neg = *p == '-';
        if (*p == '-' || *p == '+') p++;
        double n = 0;
        int digits = 0, last_was_digit = 0;
        for (; *p && !isspace((unsigned char)*p); p++) {
            if (isdigit((unsigned char)*p)) {
                n = n * 10 + (*p - '0');
                digits++;
                last_was_digit = 1;
            } else if (*p == '_' && last_was_digit && isdigit((unsigned char)p[1])) {
                last_was_digit = 0;
            } else {
                digits = 0;
                break;
            }
        }
        while (isspace((unsigned char)*p)) p++;
        if (digits && !*p) return cJSON_CreateNumber(neg ? -n : n);
        pj_fail(err, "invalid literal for int(): '%.40s'", v->valuestring);
        return NULL;
    }
    pj_fail(err, "int() argument must be a string or a number, not %s", v ? (cJSON_IsNull(v) ? "None" : "a list/dict") : "missing");
    return NULL;
}

void pj_key_str(const cJSON *v, char *buf, size_t size)
{
    if (cJSON_IsString(v)) snprintf(buf, size, "%s", v->valuestring);
    else if (!v || cJSON_IsNull(v)) snprintf(buf, size, "null");
    else if (cJSON_IsBool(v)) snprintf(buf, size, cJSON_IsTrue(v) ? "true" : "false");
    else if (cJSON_IsNumber(v) && v->valuedouble == trunc(v->valuedouble) && fabs(v->valuedouble) < 1e15)
        snprintf(buf, size, "%.0f", v->valuedouble);
    else if (cJSON_IsNumber(v)) snprintf(buf, size, "%.17g", v->valuedouble);
    else snprintf(buf, size, "?");
}

void pj_set(cJSON *obj, const char *key, cJSON *value)
{
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    else cJSON_AddItemToObject(obj, key, value);
}
