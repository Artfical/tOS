#ifndef IPSEC_H
#define IPSEC_H

#include <stdint.h>
#include "ip.h"

/* IANA protocol numbers */
#define IPPROTO_AH  51
#define IPPROTO_ESP 50

/*
 * Authentication Header (RFC 4302)
 * The ICV (Integrity Check Value) follows and has variable length
 * determined by the HMAC algorithm negotiated (not parsed here).
 */
typedef struct {
    uint8_t  next_header;  /* protocol of the encapsulated payload */
    uint8_t  payload_len;  /* (length / 4) - 2 */
    uint16_t reserved;
    uint32_t spi;          /* Security Parameters Index */
    uint32_t seq_num;
    /* ICV follows: (payload_len + 2) * 4 - 12 bytes */
} __attribute__((packed)) ipsec_ah_hdr_t;

/*
 * Encapsulating Security Payload (RFC 4303)
 * The IV, encrypted payload, padding, next_header, and ICV all follow
 * the fixed 8-byte prefix and are algorithm-dependent.
 */
typedef struct {
    uint32_t spi;
    uint32_t seq_num;
    /* IV + encrypted payload + padding + pad_len + next_header + ICV */
} __attribute__((packed)) ipsec_esp_hdr_t;

/* Simplified SA (Security Association) entry */
#define IPSEC_SA_MAX 8
typedef struct {
    int      valid;
    uint32_t peer_ip;
    uint32_t spi;
    uint8_t  protocol;  /* IPPROTO_AH or IPPROTO_ESP */
    uint32_t seq;
} ipsec_sa_t;

extern ipsec_sa_t ipsec_sa_table[IPSEC_SA_MAX];

/* Add a Security Association entry manually */
int  ipsec_sa_add(uint32_t peer_ip, uint32_t spi, uint8_t proto);
void ipsec_sa_remove(uint32_t spi);

/* Packet handlers — parse headers and pass inner payload to ip_handle */
void ipsec_ah_handle(ip_hdr_t *ip, void *pkt, int len);
void ipsec_esp_handle(ip_hdr_t *ip, void *pkt, int len);

/* Diagnostic */
void ipsec_dump_sa(void);

#endif
