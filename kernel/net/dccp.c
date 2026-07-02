#include "dccp.h"
#include "ip.h"
#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

/* -----------------------------------------------------------------------
 * Connection state (single connection, like TCP impl)
 * ----------------------------------------------------------------------- */
static struct {
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint64_t local_seq;   /* 48-bit sequence number, extended form */
    uint64_t peer_seq;
    /* rx buffer */
    uint8_t *rx_buf;
    int      rx_len;
    int      rx_closed;
} dconn;

static int dconn_active = 0;

/* -----------------------------------------------------------------------
 * Pseudo-header checksum (same pattern as UDP/TCP)
 * ----------------------------------------------------------------------- */
static uint16_t dccp_checksum(uint32_t src_ip, uint32_t dst_ip,
                               const uint8_t *seg, int seg_len) {
    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    } __attribute__((packed)) pseudo;
    pseudo.src   = src_ip;
    pseudo.dst   = dst_ip;
    pseudo.zero  = 0;
    pseudo.proto = IPPROTO_DCCP;
    pseudo.len   = htons((uint16_t)seg_len);

    uint32_t sum = 0;
    uint16_t *p  = (uint16_t *)&pseudo;
    for (int i = 0; i < (int)sizeof(pseudo) / 2; i++) sum += ntohs(p[i]);
    p = (uint16_t *)seg;
    for (int i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    if (seg_len & 1) sum += ((uint8_t *)seg)[seg_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

/* -----------------------------------------------------------------------
 * Build and send a DCCP packet (short-sequence form, X=0)
 * data_offset in 32-bit words (minimum 3 for 12-byte header)
 * ----------------------------------------------------------------------- */
static int dccp_send_pkt(uint8_t pkt_type, const void *payload, int plen) {
    /* 12-byte short header */
    int total = 12 + plen;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    /* src_port */
    *(uint16_t *)(buf + 0) = htons(dconn.src_port);
    /* dst_port */
    *(uint16_t *)(buf + 2) = htons(dconn.dst_port);
    /* data_offset (12 bytes = 3 words), CCVal=0, CsCov=0 */
    buf[4] = 3;
    buf[5] = 0;
    /* checksum placeholder */
    *(uint16_t *)(buf + 6) = 0;
    /* res(5b) | type(4b) | X(1b)=0 | seq_hi(2b) */
    buf[8]  = (uint8_t)((pkt_type & 0x0F) << 1);
    buf[9]  = 0;
    /* seq_lo (16 bits of the 24-bit short sequence) */
    *(uint16_t *)(buf + 10) = htons((uint16_t)(dconn.local_seq & 0xFFFF));

    if (plen > 0) memcpy(buf + 12, payload, plen);

    *(uint16_t *)(buf + 6) = dccp_checksum(net_ip, dconn.dst_ip, buf, total);
    dconn.local_seq++;

    int r = ip_send(dconn.dst_ip, IPPROTO_DCCP, buf, total);
    free(buf);
    return r;
}

/* -----------------------------------------------------------------------
 * Public API — connect (blocking handshake)
 * ----------------------------------------------------------------------- */
int dccp_connect(uint32_t dst_ip, uint16_t dst_port) {
    if (dconn_active) return -1;
    memset(&dconn, 0, sizeof(dconn));
    dconn.dst_ip   = dst_ip;
    dconn.dst_port = dst_port;
    dconn.src_port = 49300;
    dconn.local_seq = 1;
    dconn.state = DCCP_STATE_REQUEST;
    dconn_active = 1;

    /* Send DCCP-Request */
    /* Service code (4 bytes) required in Request packet payload */
    uint32_t svc = htonl(0);
    if (dccp_send_pkt(DCCP_PKT_REQUEST, &svc, 4) != 0) {
        dconn_active = 0; return -1;
    }

    /* Poll for DCCP-Response */
    for (int retry = 0; retry < 400; retry++) {
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IP) {
                uint8_t *ipd = pkt + sizeof(eth_hdr_t);
                int iplen    = plen - sizeof(eth_hdr_t);
                ip_hdr_t *ip = (ip_hdr_t *)ipd;
                int ihl      = (ip->ver_ihl & 0x0F) * 4;
                if (ip->protocol == IPPROTO_DCCP && ip->src_ip == dst_ip)
                    dccp_handle(ip, ipd + ihl, iplen - ihl);
            }
        }
        if (dconn.state == DCCP_STATE_OPEN) return 0;
    }
    dconn_active = 0;
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API — send data
 * ----------------------------------------------------------------------- */
int dccp_send(const void *data, int len) {
    if (!dconn_active || dconn.state != DCCP_STATE_OPEN) return -1;
    return dccp_send_pkt(DCCP_PKT_DATA, data, len);
}

/* -----------------------------------------------------------------------
 * Public API — recv (blocking poll)
 * ----------------------------------------------------------------------- */
int dccp_recv(uint8_t *buf, int max_len) {
    if (!dconn_active) return -1;
    for (;;) {
        if (dconn.rx_len > 0) {
            int n = dconn.rx_len < max_len ? dconn.rx_len : max_len;
            memcpy(buf, dconn.rx_buf, n);
            dconn.rx_len -= n;
            if (dconn.rx_len > 0)
                memmove(dconn.rx_buf, dconn.rx_buf + n, dconn.rx_len);
            else { free(dconn.rx_buf); dconn.rx_buf = 0; }
            return n;
        }
        if (dconn.rx_closed) return 0;
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_IP) {
                uint8_t *ipd = pkt + sizeof(eth_hdr_t);
                int iplen    = plen - sizeof(eth_hdr_t);
                ip_hdr_t *ip = (ip_hdr_t *)ipd;
                int ihl      = (ip->ver_ihl & 0x0F) * 4;
                if (ip->protocol == IPPROTO_DCCP)
                    dccp_handle(ip, ipd + ihl, iplen - ihl);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Public API — close
 * ----------------------------------------------------------------------- */
void dccp_close(void) {
    if (!dconn_active) return;
    if (dconn.state == DCCP_STATE_OPEN) {
        dccp_send_pkt(DCCP_PKT_CLOSE, 0, 0);
        dconn.state = DCCP_STATE_CLOSING;
    }
    if (dconn.rx_buf) { free(dconn.rx_buf); dconn.rx_buf = 0; }
    dconn.rx_len = 0;
    dconn_active = 0;
}

/* -----------------------------------------------------------------------
 * Incoming handler (called from ip_handle)
 * ----------------------------------------------------------------------- */
void dccp_handle(ip_hdr_t *ip, void *pkt, int len) {
    if (!dconn_active) return;
    if (len < 12) return;

    uint8_t *h     = (uint8_t *)pkt;
    uint16_t dport = ntohs(*(uint16_t *)(h + 0));
    uint16_t sport = ntohs(*(uint16_t *)(h + 2));
    uint8_t  doff  = h[4];
    uint8_t  type  = (h[8] >> 1) & 0x0F;
    int      hlen  = doff * 4;

    if (sport != dconn.dst_port || dport != dconn.src_port) return;
    if (ip->src_ip != dconn.dst_ip) return;

    int dlen = len - hlen;
    uint8_t *data = h + hlen;

    switch (type) {
    case DCCP_PKT_RESPONSE:
        if (dconn.state == DCCP_STATE_REQUEST) {
            /* Send DCCP-Ack to complete handshake */
            dccp_send_pkt(DCCP_PKT_ACK, 0, 0);
            dconn.state = DCCP_STATE_OPEN;
        }
        break;

    case DCCP_PKT_DATA:
    case DCCP_PKT_DATAACK:
        if (dconn.state == DCCP_STATE_OPEN && dlen > 0) {
            uint8_t *tmp = (uint8_t *)malloc(dconn.rx_len + dlen);
            if (tmp) {
                if (dconn.rx_len > 0) memcpy(tmp, dconn.rx_buf, dconn.rx_len);
                memcpy(tmp + dconn.rx_len, data, dlen);
                free(dconn.rx_buf);
                dconn.rx_buf = tmp;
                dconn.rx_len += dlen;
            }
        }
        break;

    case DCCP_PKT_CLOSEREQ:
    case DCCP_PKT_CLOSE:
        dccp_send_pkt(DCCP_PKT_RESET, 0, 0);
        dconn.rx_closed = 1;
        dconn.state     = DCCP_STATE_CLOSED;
        break;

    case DCCP_PKT_RESET:
        dconn.rx_closed = 1;
        dconn.state     = DCCP_STATE_CLOSED;
        break;

    default:
        break;
    }
}
