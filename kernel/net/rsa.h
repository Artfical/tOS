#ifndef RSA_H
#define RSA_H

#include <stdint.h>

/* RSA public-key operations for moduli of 2048..4096 bits (Montgomery
 * arithmetic, no division). The older bignum.c only handles exactly 2048 bits,
 * which is not enough to verify certificate chains (ISRG Root X1 and many
 * other roots are 4096-bit). */

#define RSA_MIN_BYTES 256    /* 2048 bits: shorter keys are refused */
#define RSA_MAX_BYTES 512    /* 4096 bits */

enum { RSA_HASH_SHA256 = 1, RSA_HASH_SHA384 = 2, RSA_HASH_SHA512 = 3 };

/* out = in^e mod mod. mod, in and out are big-endian and mod_len bytes long;
 * mod must be odd with a non-zero leading byte, in must be < mod, e odd >= 3.
 * Returns 0, or -1 on invalid input. */
int rsa_public_op(const uint8_t *mod, int mod_len, uint32_t e,
                  const uint8_t *in, int in_len, uint8_t *out);

/* RSASSA-PKCS1-v1_5 verification of `sig` over a precomputed digest. The whole
 * encoded message is rebuilt and compared byte for byte (no lenient DigestInfo
 * parsing, which is what Bleichenbacher-style forgeries rely on).
 * Returns 0 when the signature is valid, -1 otherwise. */
int rsa_pkcs1_verify(const uint8_t *mod, int mod_len, uint32_t e, int hash,
                     const uint8_t *digest, const uint8_t *sig, int sig_len);

/* RSAES-PKCS1-v1_5 encryption (TLS RSA key exchange). out is mod_len bytes. */
int rsa_pkcs1_encrypt(const uint8_t *mod, int mod_len, uint32_t e,
                      const uint8_t *msg, int msg_len, uint8_t *out);

#endif
