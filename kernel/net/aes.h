#ifndef AES_H
#define AES_H
#include <stdint.h>

void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif
