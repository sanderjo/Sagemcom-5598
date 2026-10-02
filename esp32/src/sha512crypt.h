// SHA-512 helpers for the router login (port of the Python client's
// _sha512_crypt / _sha512_hex, which mirror the router's own JS).
#pragma once
#include <stddef.h>
#include "esp_err.h"

#define SHA512_HEX_LEN 128
// "<salt>$<86 chars>", salt is 8..16 bytes
#define SHA512_CRYPT_MAX 104

esp_err_t sha512_hex(const char *text, char out[SHA512_HEX_LEN + 1]);
esp_err_t sha512_crypt(const char *password, const char *salt, char out[SHA512_CRYPT_MAX]);
