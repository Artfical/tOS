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

/* DCCP generic header (short form, X=0, 12 bytes) */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  data_offset;  /* in 32-bit words */
    uint8_t  ccval_cscov;  /* upper 4: CCVal, lower 4: CsCov */
    uint16_t checksum;
    uint8_t  res_type_x;   /* bits 7-3: reserved, bits 2-0: type (upper), X=0 */
    uint8_t  type_seq_hi;  /* type (lower), seq_hi */
    uint16_t seq_lo;
} __attribute__((packed)) dccp_hdr_t;

/* DCCP extended seq (X=1, 16 bytes) */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  data_offset;
    uint8_t  ccval_cscov;
    uint16_t checksum;
    uint8_t  res_type_x;
    uint8_t  reserved;
    uint32_t seq_hi;
    uint16_t seq_lo;
} __attribute__((packed)) dccp_hdr_ext_t;

/* Public API */
int  dccp_connect(uint32_t dst_ip, uint16_t dst_port);
int  dccp_send(const void *data, int len);
int  dccp_recv(uint8_t *buf, int max_len);
void dccp_close(void);
void dccp_handle(ip_hdr_t *ip, void *pkt, int len);

#endif
