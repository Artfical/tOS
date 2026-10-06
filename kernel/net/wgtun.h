#ifndef WGTUN_H
#define WGTUN_H

#include <stdint.h>
#include "ip.h"

/*
 * Minimal WireGuard-inspired encrypted tunnel.
 * Uses XChaCha20-Poly1305 for data encryption.
 *
 * Packet format (UDP-based, dest port configurable):
 *   [peer_id:4][nonce:24][ AEAD( sequence:8 big-endian || ip_packet ) ][poly1305_tag:16]
 * The nonce is random per packet (a 192-bit XChaCha nonce makes collisions
 * impossible in practice, and it stays safe across restarts, which a counter
 * starting at 1 is not). The sequence number inside the authenticated payload
 * drives a 64-packet anti-replay window; it starts from the wall clock so that a
 * restarted peer does not fall behind the window its partner remembers.
 *
 * Key exchange is out-of-scope (pre-shared keys only, like WireGuard PSK mode).
 */

#define WGTUN_MAX        4
#define WGTUN_KEY_LEN   32
#define WGTUN_NONCE_LEN 24

#define IPPROTO_UDP     17
#define WGTUN_PORT      51820  /* default WireGuard UDP port */

typedef struct {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint8_t  preshared_key[WGTUN_KEY_LEN];
    uint32_t peer_id;
    uint64_t tx_seq;         /* sequence number of the next packet we send */
    uint64_t rx_max;         /* highest authenticated sequence number received */
    uint64_t rx_window;      /* bit i: sequence rx_max - i was already accepted */
    int      valid;
} wgtun_t;

void wgtun_init(void);
int  wgtun_add(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
               const uint8_t psk[WGTUN_KEY_LEN], uint32_t peer_id);
int  wgtun_del(int idx);
void wgtun_list(void);

/* Encrypt and send an inner IP packet through tunnel idx */
int  wgtun_send(int idx, const void *inner_ip, int inner_len);

/* Does a tunnel listen on this local UDP port? (udp_handle() hands such datagrams to wgtun_rx) */
int  wgtun_owns_port(uint16_t port);

/* Called from udp_handle() when a WireGuard packet arrives */
void wgtun_rx(uint32_t src_ip, uint16_t src_port,
              const uint8_t *data, int len);

#endif
