#ifndef SHA256_H
#define SHA256_H
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint32_t len;
    uint64_t bits;
} sha256_t;

void sha256_init  (sha256_t *s);
void sha256_update(sha256_t *s, const uint8_t *data, uint32_t len);
void sha256_final (sha256_t *s, uint8_t out[32]);
void sha256_hash  (const uint8_t *data, uint32_t len, uint8_t out[32]);
void hmac_sha256  (const uint8_t *key, uint32_t klen,
                   const uint8_t *msg, uint32_t mlen, uint8_t out[32]);

#endif
