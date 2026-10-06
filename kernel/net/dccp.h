#ifndef DCCP_H
#define DCCP_H

#include <stdint.h>
#include "ip.h"

/* IANA protocol number */
#define IPPROTO_DCCP 33

/* DCCP packet types */
#define DCCP_PKT_REQUEST  0
#define DCCP_PKT_RESPONSE 1
#define DCCP_PKT_DATA     2
#define DCCP_PKT_ACK      3
#define DCCP_PKT_DATAACK  4
#define DCCP_PKT_CLOSEREQ 5
#define DCCP_PKT_CLOSE    6
#define DCCP_PKT_RESET    7
#define DCCP_PKT_SYNC     8
#define DCCP_PKT_SYNCACK  9

/* DCCP connection states */
#define DCCP_STATE_CLOSED      0
#define DCCP_STATE_REQUEST     1
#define DCCP_STATE_RESPOND     2
#define DCCP_STATE_PARTOPEN    3
#define DCCP_STATE_OPEN        4
#define DCCP_STATE_CLOSEREQ    5
#define DCCP_STATE_CLOSING     6
#define DCCP_STATE_TIMEWAIT    7

/* Packets are built and parsed byte-wise (see dccp.c): the generic header is 16
 * bytes with X=1 (48-bit sequence numbers, mandatory for Request/Response/Close/
 * Reset, RFC 4340 5.1) or 12 bytes with X=0 (24-bit, Data/Ack/DataAck only),
 * followed by the type-specific part (acknowledgement number subheader, service
 * code, reset code) and options. */

/* Public API */
int  dccp_connect(uint32_t dst_ip, uint16_t dst_port);          /* service code 0 */
int  dccp_connect_service(uint32_t dst_ip, uint16_t dst_port, uint32_t service_code);
/* dccp_recv(): nothing arrived for 10 s */
#define DCCP_ERR_TIMEOUT -2
int  dccp_send(const void *data, int len);
int  dccp_recv(uint8_t *buf, int max_len);
void dccp_close(void);
void dccp_handle(ip_hdr_t *ip, void *pkt, int len);

#endif
