#ifndef X25519_H
#define X25519_H

#include <stdint.h>

/* X25519 (RFC 7748): the elliptic-curve Diffie-Hellman every modern TLS client offers.
 * It is what gives a connection forward secrecy -- the server's long-term RSA key signs
 * the exchange but never encrypts it, so recovering that key later does not decrypt
 * recorded traffic, which is exactly what plain RSA key transport fails at. */

#define X25519_LEN 32

/* public = scalar * basepoint */
void x25519_base(uint8_t public_out[32], const uint8_t scalar[32]);

/* shared = scalar * peer_public. Returns -1 (and leaves nothing usable) when the peer
 * sent a point of small order, whose shared secret would be a fixed value. */
int  x25519(uint8_t shared_out[32], const uint8_t scalar[32], const uint8_t peer_public[32]);

#endif
