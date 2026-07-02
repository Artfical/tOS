#ifndef SCTP_H
#define SCTP_H

#include <stdint.h>
#include "ip.h"

/* IANA protocol number */
#define IPPROTO_SCTP 132

/* SCTP chunk types */
#define SCTP_CHUNK_DATA         0
#define SCTP_CHUNK_INIT         1
#define SCTP_CHUNK_INIT_ACK     2
#define SCTP_CHUNK_SACK         3
#define SCTP_CHUNK_HEARTBEAT    4
#define SCTP_CHUNK_HEARTBEAT_ACK 5
#define SCTP_CHUNK_ABORT        6
#define SCTP_CHUNK_SHUTDOWN     7
#define SCTP_CHUNK_SHUTDOWN_ACK 8
#define SCTP_CHUNK_COOKIE_ECHO  10
#define SCTP_CHUNK_COOKIE_ACK   11
#define SCTP_CHUNK_SHUTDOWN_COMPLETE 14

/* SCTP DATA chunk flags */
#define SCTP_DATA_LAST_FRAG  0x01
#define SCTP_DATA_FIRST_FRAG 0x02
#define SCTP_DATA_UNORDERED  0x04

/* SCTP association states */
#define SCTP_STATE_CLOSED       0
#define SCTP_STATE_COOKIE_WAIT  1
#define SCTP_STATE_COOKIE_ECHO  2
#define SCTP_STATE_ESTABLISHED  3
#define SCTP_STATE_SHUTDOWN     4

/* Common SCTP header (12 bytes) */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t vtag;       /* verification tag */
    uint32_t checksum;   /* CRC32c */
} __attribute__((packed)) sctp_hdr_t;

/* Generic chunk header (4 bytes) */
typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;     /* includes this 4-byte header */
} __attribute__((packed)) sctp_chunk_hdr_t;

/* INIT chunk (after chunk header) */
typedef struct {
    uint32_t initiate_tag;
    uint32_t a_rwnd;
    uint16_t num_outbound;
    uint16_t num_inbound;
    uint32_t init_tsn;
    /* optional parameters follow */
} __attribute__((packed)) sctp_init_t;

/* DATA chunk (after chunk header) */
typedef struct {
    uint32_t tsn;
    uint16_t stream_id;
    uint16_t stream_seq;
    uint32_t proto_id;
    /* user data follows */
} __attribute__((packed)) sctp_data_t;

/* SACK chunk (after chunk header) */
typedef struct {
    uint32_t cum_tsn_ack;
    uint32_t a_rwnd;
    uint16_t num_gap_blocks;
    uint16_t num_dup_tsns;
} __attribute__((packed)) sctp_sack_t;

/* Public API */
int  sctp_connect(uint32_t dst_ip, uint16_t dst_port);
int  sctp_send(const void *data, int len);
int  sctp_recv(uint8_t *buf, int max_len);
void sctp_close(void);
void sctp_handle(ip_hdr_t *ip, void *pkt, int len);

/* Read-only query API (for Network Monitor GUI) */
typedef struct {
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
} sctp_info_t;
int sctp_get_info(sctp_info_t *out);

#endif
