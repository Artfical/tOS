#ifndef BIGNUM_H
#define BIGNUM_H
#include <stdint.h>

/* RSA-2048 public-key encrypt (no padding — caller applies PKCS#1 v1.5).
 * base, mod, out are 256-byte big-endian buffers. */
void rsa2048_pub_encrypt(const uint8_t base[256], const uint8_t mod[256],
                         uint32_t exp, uint8_t out[256]);

#endif
