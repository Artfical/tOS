#ifndef WGTUN_H
#define WGTUN_H

#include <stdint.h>
#include "ip.h"

/*
 * Minimal WireGuard-inspired encrypted tunnel.
 * Uses XChaCha20-Poly1305 for data encryption.
 *
 * Packet format (UDP-based, dest port configurable):
 *   [peer_id:4][nonce:24][encrypted_ip_packet][poly1305_tag:16]
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
    uint64_t tx_nonce_ctr;   /* incremented each send */
    int      valid;
} wgtun_t;

void wgtun_init(void);
int  wgtun_add(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
               const uint8_t psk[WGTUN_KEY_LEN], uint32_t peer_id);
int  wgtun_del(int idx);
void wgtun_list(void);

/* Encrypt and send an inner IP packet through tunnel idx */
int  wgtun_send(int idx, const void *inner_ip, int inner_len);

/* Called from udp_handle() when a WireGuard packet arrives */
void wgtun_rx(uint32_t src_ip, uint16_t src_port,
              const uint8_t *data, int len);

#endif
