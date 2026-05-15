#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "ip.h"

#define TCP_FLAG_FIN  1
#define TCP_FLAG_SYN  2
#define TCP_FLAG_RST  4
#define TCP_FLAG_PSH  8
#define TCP_FLAG_ACK  16

int  tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(void *data, int len);
int  tcp_recv(uint8_t *buf, int max_len);
void tcp_close(void);
void tcp_handle(ip_hdr_t *ip, void *pkt, int len);

#endif
