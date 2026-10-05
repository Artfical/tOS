#ifndef SHA512_H
#define SHA512_H

#include <stdint.h>

/* SHA-512 and SHA-384 (FIPS 180-4). X.509 certificates are routinely signed
 * with sha384WithRSAEncryption / sha512WithRSAEncryption. */
typedef struct {
    uint64_t h[8];
    uint8_t  buf[128];
    uint32_t buf_len;
    uint64_t total_lo;   /* message length in bytes (64-bit; ample for any use here) */
    int      is384;
} sha512_t;

void sha512_init(sha512_t *s);
void sha384_init(sha512_t *s);
void sha512_update(sha512_t *s, const uint8_t *data, uint32_t len);
/* Writes 64 bytes (SHA-512) or 48 bytes (SHA-384, per how s was initialised). */
void sha512_final(sha512_t *s, uint8_t *out);

void sha512_hash(const uint8_t *data, uint32_t len, uint8_t out[64]);
void sha384_hash(const uint8_t *data, uint32_t len, uint8_t out[48]);

#endif
