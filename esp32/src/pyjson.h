// Python semantics on cJSON values, for porting the Python client faithfully:
// truthiness, `a or b`, dict.get, d[key] (fails like a KeyError), int().
#pragma once
#include <stddef.h>
#include "cJSON.h"

// First failure wins; tools check `failed` once at the end.
typedef struct {
    char *msg;
    size_t size;
    int failed;
} pj_err_t;

void pj_fail(pj_err_t *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

int pj_truthy(const cJSON *v);
const cJSON *pj_or(const cJSON *a, const cJSON *b);       // `a or b` (NULL = missing = None)
const cJSON *pj_get(const cJSON *obj, const char *key);    // obj.get(key); NULL if obj is no dict
const cJSON *pj_req(const cJSON *obj, const char *key, pj_err_t *err);  // obj[key]
const cJSON *pj_item(const cJSON *arr, int index, pj_err_t *err);      // arr[index]
cJSON *pj_dup(const cJSON *v);                             // copy, or null when missing
int pj_same_key(const cJSON *a, const cJSON *b);           // equal as dict keys
int pj_is_str(const cJSON *v, const char *s);
// int(v) as a new JSON number; NULL (and err set) where Python would raise
cJSON *pj_int(const cJSON *v, pj_err_t *err);
// the JSON object key json.dumps() writes for dict key `v` ("null", "5", ...)
void pj_key_str(const cJSON *v, char *buf, size_t size);
// d[key] = value: replaces an existing key in place (keeps its position, like a dict)
void pj_set(cJSON *obj, const char *key, cJSON *value);
