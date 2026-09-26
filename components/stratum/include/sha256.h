#ifndef STRATUM_SHA256_H
#define STRATUM_SHA256_H

#include <stddef.h>
#include <stdint.h>

// Portable SHA-256. ESP-IDF 6 (mbedTLS 4) no longer exposes the legacy
// mbedtls_sha256_* API, and PSA gives no access to the internal state
// needed for midstate calculation, so carry own implementation.

typedef struct
{
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buffer[64];
    size_t buffer_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

void sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

#endif // STRATUM_SHA256_H
