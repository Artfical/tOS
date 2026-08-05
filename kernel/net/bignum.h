#ifndef BIGNUM_H
#define BIGNUM_H
#include <stdint.h>

/* RSA-2048 public-key encrypt (no padding — caller applies PKCS#1 v1.5).
 * base, mod, out are 256-byte big-endian buffers. */
void rsa2048_pub_encrypt(const uint8_t base[256], const uint8_t mod[256],
                         uint32_t exp, uint8_t out[256]);

/* General base^exp mod m for a 2048-bit base/modulus with an
 * arbitrary-length big-endian exponent (unlike rsa2048_pub_encrypt's
 * fixed uint32_t exponent) -- needed for Diffie-Hellman, where the
 * exponent is a large random secret, not a small fixed public value
 * like RSA's 65537. exp_len is exp's length in bytes. */
void bignum_modexp(const uint8_t base[256], const uint8_t exp[], uint32_t exp_len,
                   const uint8_t mod[256], uint8_t out[256]);

#endif
