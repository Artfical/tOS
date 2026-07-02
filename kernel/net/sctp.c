#include "sctp.h"
#include "ip.h"
#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

/* -----------------------------------------------------------------------
 * CRC32c (Castagnoli) — required by RFC 4960 for SCTP checksum
 * Table-driven, reflected polynomial 0x82F63B78.
 * ----------------------------------------------------------------------- */
static uint32_t crc32c_table[256];
static int crc32c_init_done = 0;

static void crc32c_init(void) {
    if (crc32c_init_done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0x82F63B78U & -(crc & 1));
        crc32c_table[i] = crc;
    }
    crc32c_init_done = 1;
}

static uint32_t crc32c(const uint8_t *buf, int len) {
    crc32c_init();
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

/* -----------------------------------------------------------------------
 * Association state
 * ----------------------------------------------------------------------- */
static struct {
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t local_vtag;     /* our verification tag sent in INIT */
    uint32_t peer_vtag;      /* peer's verification tag from INIT-ACK */
    uint32_t local_tsn;      /* next TSN to send */
    uint32_t peer_cum_tsn;   /* last cumulative TSN received from peer */
    uint8_t  cookie[64];     /* cookie echoed back from INIT-ACK */
    int      cookie_len;
    /* rx ring buffer */
    uint8_t *rx_buf;
    int      rx_len;
    int      rx_cap;
} assoc;

static int assoc_active = 0;

/* -----------------------------------------------------------------------
 * Packet build helpers
 * ----------------------------------------------------------------------- */
static int sctp_send_raw(void *payload, int payload_len) {
    uint32_t vtag = (assoc.state >= SCTP_STATE_COOKIE_ECHO)
                    ? assoc.peer_vtag : 0;
    int total = sizeof(sctp_hdr_t) + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    sctp_hdr_t *hdr = (sctp_hdr_t *)buf;
    hdr->src_port = htons(assoc.src_port);
    hdr->dst_port = htons(assoc.dst_port);
    hdr->vtag     = htonl(vtag);
    hdr->checksum = 0;
    memcpy(buf + sizeof(sctp_hdr_t), payload, payload_len);

    /* CRC32c over the whole SCTP packet (header + chunks, checksum=0) */
    hdr->checksum = htonl(crc32c(buf, total));

    int r = ip_send(assoc.dst_ip, IPPROTO_SCTP, buf, total);
    free(buf);
    return r;
}

static int build_chunk(uint8_t type, uint8_t flags,
                        const void *val, int val_len,
                        uint8_t *out) {
    int padded = (val_len + 3) & ~3;
    sctp_chunk_hdr_t *c = (sctp_chunk_hdr_t *)out;
    c->type   = type;
    c->flags  = flags;
    c->length = htons(4 + val_len);
    if (val_len) memcpy(out + 4, val, val_len);
    if (padded > val_len) memset(out + 4 + val_len, 0, padded - val_len);
    return 4 + padded;
}

/* -----------------------------------------------------------------------
 * Send INIT
 * ----------------------------------------------------------------------- */
static int send_init(void) {
    uint8_t chunk[64];
    sctp_init_t init;
    init.initiate_tag  = htonl(assoc.local_vtag);
    init.a_rwnd        = htonl(32768U);
    init.num_outbound  = htons(1);
    init.num_inbound   = htons(1);
    init.init_tsn      = htonl(assoc.local_tsn);
    int clen = build_chunk(SCTP_CHUNK_INIT, 0, &init, sizeof(init), chunk);
    return sctp_send_raw(chunk, clen);
}

/* -----------------------------------------------------------------------
 * Send COOKIE-ECHO
 * ----------------------------------------------------------------------- */
static int send_cookie_echo(void) {
    uint8_t chunk[128];
    int clen = build_chunk(SCTP_CHUNK_COOKIE_ECHO, 0,
                           assoc.cookie, assoc.cookie_len, chunk);
    return sctp_send_raw(chunk, clen);
}

/* -----------------------------------------------------------------------
 * Send SACK for a received TSN
 * ----------------------------------------------------------------------- */
static int send_sack(void) {
    uint8_t chunk[16];
    sctp_sack_t sack;
    sack.cum_tsn_ack   = htonl(assoc.peer_cum_tsn);
    sack.a_rwnd        = htonl(32768U);
    sack.num_gap_blocks = 0;
    sack.num_dup_tsns  = 0;
    int clen = build_chunk(SCTP_CHUNK_SACK, 0, &sack, sizeof(sack), chunk);
    return sctp_send_raw(chunk, clen);
}

/* -----------------------------------------------------------------------
 * Public API — connect (client side, blocking)
 * ----------------------------------------------------------------------- */
int sctp_connect(uint32_t dst_ip, uint16_t dst_port) {
    if (assoc_active) return -1;

    memset(&assoc, 0, sizeof(assoc));
    assoc.dst_ip    = dst_ip;
    assoc.dst_port  = dst_port;
    assoc.src_port  = 49200;
    assoc.local_vtag = 0xA5C3F1B2U;
    assoc.local_tsn  = 1000;
    assoc.state      = SCTP_STATE_COOKIE_WAIT;
    assoc_active     = 1;

    if (send_init() != 0) { assoc_active = 0; return -1; }

    /* Poll for INIT-ACK then COOKIE-ACK */
    for (int retry = 0; retry < 400; retry++) {
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IP) {
                uint8_t *ip_data = pkt + sizeof(eth_hdr_t);
                int ip_len = len - sizeof(eth_hdr_t);
                ip_hdr_t *ip = (ip_hdr_t *)ip_data;
                int ihl = (ip->ver_ihl & 0x0F) * 4;
                if (ip->protocol == IPPROTO_SCTP && ip->src_ip == dst_ip) {
                    sctp_handle(ip, ip_data + ihl, ip_len - ihl);
                }
            }
        }
        if (assoc.state == SCTP_STATE_ESTABLISHED) return 0;
    }
    assoc_active = 0;
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API — send DATA chunk
 * ----------------------------------------------------------------------- */
int sctp_send(const void *data, int len) {
    if (!assoc_active || assoc.state != SCTP_STATE_ESTABLISHED) return -1;

    int total_chunk = 4 + sizeof(sctp_data_t) + len;
    int padded      = (total_chunk + 3) & ~3;
    uint8_t *chunk  = (uint8_t *)malloc(padded);
    if (!chunk) return -1;

    sctp_chunk_hdr_t *c = (sctp_chunk_hdr_t *)chunk;
    c->type   = SCTP_CHUNK_DATA;
    c->flags  = SCTP_DATA_FIRST_FRAG | SCTP_DATA_LAST_FRAG;
    c->length = htons((uint16_t)total_chunk);

    sctp_data_t *d = (sctp_data_t *)(chunk + 4);
    uint32_t tsn_val = assoc.local_tsn++;
    d->tsn        = htonl(tsn_val);
    d->stream_id  = 0;
    d->stream_seq = 0;
    d->proto_id   = 0;
    memcpy(chunk + 4 + sizeof(sctp_data_t), data, len);
    if (padded > total_chunk) memset(chunk + total_chunk, 0, padded - total_chunk);

    int r = sctp_send_raw(chunk, padded);
    free(chunk);
    return r;
}

/* -----------------------------------------------------------------------
 * Public API — receive DATA (blocking poll)
 * ----------------------------------------------------------------------- */
int sctp_recv(uint8_t *buf, int max_len) {
    if (!assoc_active) return -1;
    for (;;) {
        if (assoc.rx_len > 0) {
            int n = assoc.rx_len < max_len ? assoc.rx_len : max_len;
            memcpy(buf, assoc.rx_buf, n);
            assoc.rx_len -= n;
            if (assoc.rx_len > 0)
                memmove(assoc.rx_buf, assoc.rx_buf + n, assoc.rx_len);
            else { free(assoc.rx_buf); assoc.rx_buf = 0; assoc.rx_cap = 0; }
            return n;
        }
        if (assoc.state != SCTP_STATE_ESTABLISHED) return 0;
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IP) {
                uint8_t *ip_data = pkt + sizeof(eth_hdr_t);
                int ip_len = plen - sizeof(eth_hdr_t);
                ip_hdr_t *ip = (ip_hdr_t *)ip_data;
                int ihl = (ip->ver_ihl & 0x0F) * 4;
                if (ip->protocol == IPPROTO_SCTP)
                    sctp_handle(ip, ip_data + ihl, ip_len - ihl);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Public API — close association
 * ----------------------------------------------------------------------- */
void sctp_close(void) {
    if (!assoc_active) return;
    if (assoc.state == SCTP_STATE_ESTABLISHED) {
        uint8_t chunk[8];
        uint32_t cum = htonl(assoc.peer_cum_tsn);
        build_chunk(SCTP_CHUNK_SHUTDOWN, 0, &cum, 4, chunk);
        sctp_send_raw(chunk, 8);
    }
    assoc.state = SCTP_STATE_CLOSED;
    if (assoc.rx_buf) { free(assoc.rx_buf); assoc.rx_buf = 0; }
    assoc_active = 0;
}

/* -----------------------------------------------------------------------
 * Incoming packet handler (called from ip_handle)
 * ----------------------------------------------------------------------- */
void sctp_handle(ip_hdr_t *ip, void *pkt, int len) {
    if (len < (int)sizeof(sctp_hdr_t)) return;
    sctp_hdr_t *hdr = (sctp_hdr_t *)pkt;

    if (!assoc_active) return;
    if (ntohs(hdr->dst_port) != assoc.src_port) return;
    if (ip->src_ip != assoc.dst_ip) return;

    /* Validate CRC32c */
    uint32_t recv_crc = ntohl(hdr->checksum);
    hdr->checksum = 0;
    if (crc32c((uint8_t *)pkt, len) != recv_crc) {
        hdr->checksum = htonl(recv_crc);
        return;
    }
    hdr->checksum = htonl(recv_crc);

    /* Walk chunks */
    uint8_t *pos = (uint8_t *)pkt + sizeof(sctp_hdr_t);
    int rem      = len - sizeof(sctp_hdr_t);

    while (rem >= 4) {
        sctp_chunk_hdr_t *c = (sctp_chunk_hdr_t *)pos;
        int clen  = ntohs(c->length);
        int padded = (clen + 3) & ~3;
        if (clen < 4 || clen > rem) break;

        uint8_t *val = pos + 4;
        int vlen     = clen - 4;

        switch (c->type) {
        case SCTP_CHUNK_INIT_ACK:
            if (assoc.state == SCTP_STATE_COOKIE_WAIT && vlen >= (int)sizeof(sctp_init_t)) {
                sctp_init_t *ia = (sctp_init_t *)val;
                assoc.peer_vtag = ntohl(ia->initiate_tag);
                /* look for State Cookie parameter (type 0x0007) */
                uint8_t *opt = val + sizeof(sctp_init_t);
                int optrem   = vlen - sizeof(sctp_init_t);
                while (optrem >= 4) {
                    uint16_t otype = ntohs(*(uint16_t *)opt);
                    uint16_t olen  = ntohs(*(uint16_t *)(opt + 2));
                    if (olen < 4 || olen > optrem) break;
                    if (otype == 0x0007) { /* State Cookie */
                        int cl = olen - 4;
                        if (cl > (int)sizeof(assoc.cookie)) cl = sizeof(assoc.cookie);
                        memcpy(assoc.cookie, opt + 4, cl);
                        assoc.cookie_len = cl;
                    }
                    int op = (olen + 3) & ~3;
                    opt    += op;
                    optrem -= op;
                }
                assoc.state = SCTP_STATE_COOKIE_ECHO;
                send_cookie_echo();
            }
            break;

        case SCTP_CHUNK_COOKIE_ACK:
            if (assoc.state == SCTP_STATE_COOKIE_ECHO)
                assoc.state = SCTP_STATE_ESTABLISHED;
            break;

        case SCTP_CHUNK_DATA:
            if (assoc.state == SCTP_STATE_ESTABLISHED && vlen >= (int)sizeof(sctp_data_t)) {
                sctp_data_t *d = (sctp_data_t *)val;
                uint32_t tsn   = ntohl(d->tsn);
                int dlen       = vlen - sizeof(sctp_data_t);
                uint8_t *data  = val + sizeof(sctp_data_t);
                assoc.peer_cum_tsn = tsn;
                /* append to rx buffer */
                if (dlen > 0) {
                    uint8_t *tmp = (uint8_t *)malloc(assoc.rx_len + dlen);
                    if (tmp) {
                        if (assoc.rx_len > 0) memcpy(tmp, assoc.rx_buf, assoc.rx_len);
                        memcpy(tmp + assoc.rx_len, data, dlen);
                        free(assoc.rx_buf);
                        assoc.rx_buf = tmp;
                        assoc.rx_len += dlen;
                        assoc.rx_cap  = assoc.rx_len;
                    }
                }
                send_sack();
            }
            break;

        case SCTP_CHUNK_HEARTBEAT: {
            /* Echo back as HEARTBEAT-ACK */
            uint8_t ack_chunk[256];
            int al = vlen < 248 ? vlen : 248;
            build_chunk(SCTP_CHUNK_HEARTBEAT_ACK, 0, val, al, ack_chunk);
            sctp_send_raw(ack_chunk, 4 + al);
            break;
        }

        case SCTP_CHUNK_SHUTDOWN:
            if (assoc.state == SCTP_STATE_ESTABLISHED) {
                assoc.state = SCTP_STATE_SHUTDOWN;
                uint8_t ack[4];
                build_chunk(SCTP_CHUNK_SHUTDOWN_ACK, 0, 0, 0, ack);
                sctp_send_raw(ack, 4);
            }
            break;

        case SCTP_CHUNK_SHUTDOWN_COMPLETE:
            assoc.state = SCTP_STATE_CLOSED;
            assoc_active = 0;
            break;

        case SCTP_CHUNK_ABORT:
            assoc.state  = SCTP_STATE_CLOSED;
            assoc_active = 0;
            break;

        default:
            break;
        }

        pos += padded;
        rem -= padded;
    }
}

/* Read-only query API for Network Monitor */
int sctp_get_info(sctp_info_t *out)
{
    if (!assoc_active) return 0;
    out->state    = assoc.state;
    out->dst_ip   = assoc.dst_ip;
    out->dst_port = assoc.dst_port;
    out->src_port = assoc.src_port;
    return 1;
}
