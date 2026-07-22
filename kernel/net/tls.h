#ifndef TLS_H
#define TLS_H

#include <stdint.h>

/* TLS 1.2 minimal client context */
#define TLS_RX_BUF  8192
#define TLS_TX_BUF  4096

typedef struct {
    int      fd;
    uint8_t  client_rand[32];
    uint8_t  server_rand[32];
    uint8_t  master[48];
    uint8_t  client_write_key[16];
    uint8_t  server_write_key[16];
    uint8_t  client_write_iv[16];
    uint8_t  server_write_iv[16];
    uint8_t  client_mac[32];
    uint8_t  server_mac[32];
    uint64_t tx_seq;
    uint64_t rx_seq;
    int      handshake_done;
    /* handshake transcript for Finished */
    uint8_t  hs_buf[8192];
    uint32_t hs_len;
    /* raw receive buffer */
    uint8_t  rx_raw[TLS_RX_BUF];
    uint32_t rx_raw_len;
    /* decrypted plaintext buffer */
    uint8_t  rx_plain[TLS_RX_BUF];
    uint32_t rx_plain_len;
    uint32_t rx_plain_pos;
} tls_ctx_t;

/* tls_connect()'s negative return codes -- numbered well past every
 * other layer's range (see tcp.h). A failed TCP connect propagates
 * tcp_connect()'s own code verbatim (an ARP_ERR_, IP_ERR_NOMEM, or
 * TCP_ERR_ value), so that case reports the real underlying reason
 * instead of a single generic "handshake or connection error" that
 * couldn't tell "the SYN never got a reply" apart from "the server
 * rejected our handshake". */
#define TLS_ERR_ALERT     -50 /* server sent a fatal alert (see dmesg for level/description) */
#define TLS_ERR_HANDSHAKE -51 /* handshake failed after TCP connected -- see dmesg for which step */

/* Connect and perform TLS 1.2 handshake */
int  tls_connect(tls_ctx_t *ctx, uint32_t ip, uint16_t port);
const char *tls_connect_strerror(int err);
/* Write application data */
int  tls_write(tls_ctx_t *ctx, const uint8_t *data, int len);
/* Read decrypted application data */
int  tls_read(tls_ctx_t *ctx, uint8_t *buf, int max);
/* Close TLS connection */
void tls_close(tls_ctx_t *ctx);

#endif
