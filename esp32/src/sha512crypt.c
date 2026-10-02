#include "sha512crypt.h"
#include <stdio.h>
#include <string.h>
#include "psa/crypto.h"

#define DIGEST_LEN 64
#define ROUNDS 5000

static const char B64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// Byte read-order used by the router's JS when base64-encoding the final digest.
static const uint8_t DIGEST_BYTE_ORDER[DIGEST_LEN] = {
    42, 21, 0, 1, 43, 22, 23, 2, 44, 45, 24, 3, 4, 46, 25, 26, 5, 47, 48, 27,
    6, 7, 49, 28, 29, 8, 50, 51, 30, 9, 10, 52, 31, 32, 11, 53, 54, 33, 12,
    13, 55, 34, 35, 14, 56, 57, 36, 15, 16, 58, 37, 38, 17, 59, 60, 39, 18,
    19, 61, 40, 41, 20, 62, 63,
};

static psa_status_t h_begin(psa_hash_operation_t *op)
{
    *op = psa_hash_operation_init();
    return psa_hash_setup(op, PSA_ALG_SHA_512);
}

static void h_add(psa_hash_operation_t *op, const void *data, size_t len)
{
    if (len) psa_hash_update(op, data, len);
}

// digest repeated to `length` bytes (Python's _sequence)
static void h_add_seq(psa_hash_operation_t *op, const uint8_t *digest, size_t length)
{
    for (; length >= DIGEST_LEN; length -= DIGEST_LEN) h_add(op, digest, DIGEST_LEN);
    h_add(op, digest, length);
}

static psa_status_t h_end(psa_hash_operation_t *op, uint8_t out[DIGEST_LEN])
{
    size_t n;
    return psa_hash_finish(op, out, DIGEST_LEN, &n);
}

esp_err_t sha512_hex(const char *text, char out[SHA512_HEX_LEN + 1])
{
    uint8_t d[DIGEST_LEN];
    size_t n;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_512, (const uint8_t *)text, strlen(text), d, sizeof(d), &n) != PSA_SUCCESS)
        return ESP_FAIL;
    for (int i = 0; i < DIGEST_LEN; i++) sprintf(out + 2 * i, "%02x", d[i]);
    return ESP_OK;
}

esp_err_t sha512_crypt(const char *password, const char *salt, char out[SHA512_CRYPT_MAX])
{
    const size_t plen = strlen(password), slen = strlen(salt);
    if (slen < 8 || slen > 16 || psa_crypto_init() != PSA_SUCCESS) return ESP_ERR_INVALID_ARG;

    psa_hash_operation_t op;
    uint8_t digest_b[DIGEST_LEN], digest_a[DIGEST_LEN], dp[DIGEST_LEN], ds[DIGEST_LEN], digest[DIGEST_LEN];
    uint8_t seq_p[256], seq_s[16];
    if (plen > sizeof(seq_p)) return ESP_ERR_INVALID_SIZE;

    h_begin(&op);
    h_add(&op, password, plen);
    h_add(&op, salt, slen);
    h_add(&op, password, plen);
    h_end(&op, digest_b);

    h_begin(&op);
    h_add(&op, password, plen);
    h_add(&op, salt, slen);
    h_add_seq(&op, digest_b, plen);
    for (size_t n = plen; n > 0; n >>= 1) {
        if (n & 1) h_add(&op, digest_b, DIGEST_LEN);
        else h_add(&op, password, plen);
    }
    h_end(&op, digest_a);

    h_begin(&op);
    for (size_t i = 0; i < plen; i++) h_add(&op, password, plen);
    h_end(&op, dp);
    for (size_t i = 0; i < plen; i++) seq_p[i] = dp[i % DIGEST_LEN];

    h_begin(&op);
    for (int i = 0; i < 16 + digest_a[0]; i++) h_add(&op, salt, slen);
    h_end(&op, ds);
    memcpy(seq_s, ds, slen);

    memcpy(digest, digest_a, DIGEST_LEN);
    for (int round = 0; round < ROUNDS; round++) {
        h_begin(&op);
        if (round & 1) h_add(&op, seq_p, plen);
        else h_add(&op, digest, DIGEST_LEN);
        if (round % 3) h_add(&op, seq_s, slen);
        if (round % 7) h_add(&op, seq_p, plen);
        if (round & 1) h_add(&op, digest, DIGEST_LEN);
        else h_add(&op, seq_p, plen);
        if (h_end(&op, digest) != PSA_SUCCESS) return ESP_FAIL;
    }

    char *p = out + sprintf(out, "%s$", salt);
    for (int i = 0; i < DIGEST_LEN; i += 3) {
        uint8_t b0 = digest[DIGEST_BYTE_ORDER[i]];
        if (i + 1 >= DIGEST_LEN) {
            *p++ = B64[b0 & 0x3F];
            *p++ = B64[(b0 & 0xC0) >> 6];
        } else {
            uint8_t b1 = digest[DIGEST_BYTE_ORDER[i + 1]], b2 = digest[DIGEST_BYTE_ORDER[i + 2]];
            *p++ = B64[b0 & 0x3F];
            *p++ = B64[((b0 & 0xC0) >> 6) | ((b1 & 0x0F) << 2)];
            *p++ = B64[((b1 & 0xF0) >> 4) | ((b2 & 0x03) << 4)];
            *p++ = B64[(b2 & 0xFC) >> 2];
        }
    }
    *p = '\0';
    return ESP_OK;
}
