#ifndef GCM_H
#define GCM_H

#include <stdint.h>

/* AES-128-GCM (NIST SP 800-38D), the authenticated cipher every modern TLS suite uses.
 * The nonce is the 12-byte form TLS always uses; `aad` is the additional authenticated
 * data (TLS's record header and sequence number), which is authenticated but not encrypted. */

void aes_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *plain, uint32_t len,
                     uint8_t *cipher, uint8_t tag[16]);

/* 0 = the tag matched and `plain` holds the data; -1 = forged or corrupt (plain is then cleared).
 * The comparison is constant-time, and nothing is handed back before it passes. */
int  aes_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *cipher, uint32_t len,
                     uint8_t *plain, const uint8_t tag[16]);

#endif
