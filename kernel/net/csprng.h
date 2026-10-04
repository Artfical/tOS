#ifndef CSPRNG_H
#define CSPRNG_H

#include <stdint.h>

/* Cryptographically secure random bytes: a ChaCha20 generator with fast key
 * erasure, seeded from RDRAND (when the CPU has it), TSC/PIT timing jitter,
 * uptime and the MAC address, and re-keyed with fresh timing on every call. */
void csprng_fill(uint8_t *buf, int len);
uint32_t csprng_u32(void);
/* Mixes caller-supplied entropy (e.g. packet arrival times) into the state. */
void csprng_add_entropy(const void *data, int len);

#endif
