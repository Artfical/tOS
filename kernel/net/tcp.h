#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "ip.h"

#define TCP_FLAG_FIN  1
#define TCP_FLAG_SYN  2
#define TCP_FLAG_RST  4
#define TCP_FLAG_PSH  8
#define TCP_FLAG_ACK  16

/* TCP states (RFC 793) */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT    10

#define TCP_MAX_SOCKETS  16
#define TCP_MSS          1460
#define TCP_RETX_TIMEOUT 50
#define TCP_RETX_MAX     5

/* tcp_connect()/tcp_connect2()'s negative return codes -- numbered well
 * past every other layer's range (see dns.h/icmp.h) so a propagated
 * lower-layer code (arp_resolve()'s or ip_send()'s own, returned
 * verbatim when the SYN can't even be sent) never collides with one of
 * these. Without this, "connection refused" and "no reply to SYN" both
 * used to collapse into the same generic failure a caller couldn't
 * tell apart. */
#define TCP_ERR_NOSOCK      -40 /* no free TCP socket */
#define TCP_ERR_REFUSED     -41 /* SYN sent, got RST back (port closed / firewalled) */
#define TCP_ERR_TIMEOUT     -42 /* SYN sent, no reply before the deadline */
#define TCP_ERR_RECV_TIMEOUT -43 /* connected, but no data arrived before the deadline (peer went silent) */

/* Socket-based API */
int  tcp_socket(void);
int  tcp_connect2(int fd, uint32_t dst_ip, uint16_t dst_port);
int  tcp_send2(int fd, void *data, int len);
int  tcp_recv2(int fd, uint8_t *buf, int max_len);
void tcp_close2(int fd);
int  tcp_listen(int fd, uint16_t port);
int  tcp_accept(int fd);

/* Backward-compatible blocking API */
int  tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(void *data, int len);
int  tcp_recv(uint8_t *buf, int max_len);
void tcp_close(void);
const char *tcp_connect_strerror(int err);

/* Called by ip.c and scheduler */
void tcp_handle(ip_hdr_t *ip, void *pkt, int len);
void tcp_tick(void);

/* Read-only query API (for Network Monitor GUI) */
typedef struct {
    int      fd;
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
} tcp_conn_info_t;
int tcp_get_connections(tcp_conn_info_t *out, int max);

#endif
