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
#define IPSEC_KEY_MAX 32          /* HMAC-SHA-256 key */
#define IPSEC_AH_ICV_LEN 16       /* HMAC-SHA-256-128 (RFC 4868) */
typedef struct {
    int      valid;
    uint32_t peer_ip;
    uint32_t spi;
    uint8_t  protocol;  /* IPPROTO_AH or IPPROTO_ESP */
    uint32_t seq;       /* highest sequence number that passed authentication */
    uint64_t replay_window;   /* bit i set: sequence number (seq - i) was already accepted */
    uint8_t  key[IPSEC_KEY_MAX];
    int      key_len;   /* 0 = no key: AH packets cannot be authenticated and are dropped */
} ipsec_sa_t;

extern ipsec_sa_t ipsec_sa_table[IPSEC_SA_MAX];

/* Add a Security Association entry manually */
int  ipsec_sa_add(uint32_t peer_ip, uint32_t spi, uint8_t proto);
/* HMAC-SHA-256 key (1..32 bytes) of an existing SA; AH needs one. */
int  ipsec_sa_set_key(uint32_t peer_ip, uint32_t spi, const uint8_t *key, int key_len);
void ipsec_sa_remove(uint32_t spi);   /* removes every SA with this SPI */

/* Packet handlers — parse headers and pass inner payload to ip_handle */
void ipsec_ah_handle(ip_hdr_t *ip, void *pkt, int len);
void ipsec_esp_handle(ip_hdr_t *ip, void *pkt, int len);

/* Diagnostic */
void ipsec_dump_sa(void);

#endif
