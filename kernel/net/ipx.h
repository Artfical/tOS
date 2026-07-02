#ifndef IPX_H
#define IPX_H

#include <stdint.h>

/* EtherType for Novell IPX (802.3 raw) and IPX over Ethernet II */
#define ETHERTYPE_IPX  0x8137

/* IPX socket numbers (well-known) */
#define IPX_SOCK_NCP       0x0451
#define IPX_SOCK_SAP       0x0452
#define IPX_SOCK_RIP       0x0453
#define IPX_SOCK_NETBIOS   0x0455
#define IPX_SOCK_DIAGNOSTIC 0x0456
#define IPX_SOCK_ECHO      0x0457

/* IPX packet types */
#define IPX_TYPE_UNKNOWN   0x00
#define IPX_TYPE_RIP       0x01
#define IPX_TYPE_ECHO      0x02
#define IPX_TYPE_ERROR     0x03
#define IPX_TYPE_PEP       0x04
#define IPX_TYPE_SPX       0x05
#define IPX_TYPE_NCP       0x11
#define IPX_TYPE_NETBIOS   0x14

/* 30-byte IPX header (big-endian on wire) */
typedef struct {
    uint16_t checksum;    /* always 0xFFFF (no checksum) */
    uint16_t length;      /* header + data, big-endian   */
    uint8_t  hop_count;   /* transport control           */
    uint8_t  packet_type;
    uint8_t  dst_net[4];
    uint8_t  dst_node[6];
    uint16_t dst_sock;
    uint8_t  src_net[4];
    uint8_t  src_node[6];
    uint16_t src_sock;
} __attribute__((packed)) ipx_hdr_t;

void ipx_handle(void *pkt, int len);
int  ipx_send(const uint8_t *dst_node, const uint8_t *dst_net,
              uint16_t dst_sock, uint16_t src_sock,
              uint8_t ptype, const void *data, int data_len);

#endif
