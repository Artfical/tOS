#include "dccp.h"
#include "ip.h"
#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "csprng.h"
#include "debugmon.h"

/* DCCP (RFC 4340), client side, no congestion control / option processing (CCID
 * negotiation is left to the peer's defaults). Sequence numbers are 48-bit
 * (X=1) on everything we send. */

#define DCCP_SEQ_MASK   0xFFFFFFFFFFFFULL
#define DCCP_SEQ_WINDOW 100              /* sequence window W (RFC 4340 7.5.1, default) */
#define DCCP_RX_MAX     65536            /* received-but-unread data per connection */
#define DCCP_RESET_CLOSED 1

/* -----------------------------------------------------------------------
 * Connection state (single connection, like the TCP/SCTP clients)
 * ----------------------------------------------------------------------- */
static struct {
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t service;
    uint64_t iss;         /* sequence number of our first Request */
    uint64_t gss;         /* greatest sequence number sent */
    uint64_t gsr;         /* greatest valid sequence number received */
    int      have_gsr;
    uint8_t *rx_buf;
    int      rx_len;
    int      rx_closed;
} dconn;

static int dconn_active = 0;

/* 48-bit serial-number arithmetic */
static uint64_t seq_add(uint64_t a, uint64_t n) { return (a + n) & DCCP_SEQ_MASK; }
static int64_t seq_diff(uint64_t a, uint64_t b)          /* a - b as a signed distance */
{
    uint64_t d = (a - b) & DCCP_SEQ_MASK;
    return (d & (1ULL << 47)) ? (int64_t)d - (int64_t)(1ULL << 48) : (int64_t)d;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put48(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 6; i++) p[i] = (uint8_t)(v >> (40 - 8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t get48(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | p[i];
    return v;
}
/* -----------------------------------------------------------------------
 * Pseudo-header checksum (RFC 4340 9.1; CsCov 0: the whole packet)
 * Returns the value to store in (or compare with) the checksum field.
 * ----------------------------------------------------------------------- */
static uint16_t dccp_checksum(uint32_t src_ip, uint32_t dst_ip,
                               const uint8_t *seg, int seg_len) {
    uint8_t ps[12];
    memcpy(ps, &src_ip, 4);
    memcpy(ps + 4, &dst_ip, 4);
    ps[8] = 0;
    ps[9] = IPPROTO_DCCP;
    put16(ps + 10, (uint16_t)seg_len);

    uint32_t sum = 0;
    for (int i = 0; i + 1 < 12; i += 2) sum += (uint32_t)((ps[i] << 8) | ps[i + 1]);
    for (int i = 0; i + 1 < seg_len; i += 2) sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    if (seg_len & 1) sum += (uint32_t)seg[seg_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* -----------------------------------------------------------------------
 * Build and send a DCCP packet with the extended (X=1) generic header.
 *   [16-byte generic header][8-byte ack subheader if has_ack][extra][payload]
 * The data offset (in 32-bit words) covers everything before the payload. The
 * old code always sent a 12-byte X=0 header with offset 3: no acknowledgement
 * number where the RFC requires one (Response, Ack, Close, Reset ...), and short
 * sequence numbers on Request/Close, which the RFC forbids, so a real peer
 * dropped every packet of ours.
 * ----------------------------------------------------------------------- */
static int dccp_send_pkt(uint8_t pkt_type, int has_ack, uint64_t ack,
                         const uint8_t *extra, int extra_len,
                         const void *payload, int plen) {
    int hdr = 16 + (has_ack ? 8 : 0) + extra_len;
    if (plen < 0 || (hdr & 3) || hdr + plen > IP_MAX_PAYLOAD) return -1;
    int total = hdr + plen;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return -1;
    memset(buf, 0, (size_t)hdr);

    put16(buf + 0, dconn.src_port);
    put16(buf + 2, dconn.dst_port);
    buf[4] = (uint8_t)(hdr / 4);                  /* data offset */
    buf[5] = 0;                                   /* CCVal 0, CsCov 0 (checksum covers the whole packet) */
    buf[8] = (uint8_t)(((pkt_type & 0x0F) << 1) | 1);   /* res(3) type(4) X=1 */
    dconn.gss = seq_add(dconn.gss, 1);
    put48(buf + 10, dconn.gss);
    int off = 16;
    if (has_ack) { put48(buf + off + 2, ack); off += 8; }
    if (extra_len) memcpy(buf + off, extra, (size_t)extra_len);
    if (plen > 0) memcpy(buf + hdr, payload, (size_t)plen);

    put16(buf + 6, dccp_checksum(net_ip, dconn.dst_ip, buf, total));
    int r = ip_send(dconn.dst_ip, IPPROTO_DCCP, buf, total);
    free(buf);
    return r;
}

static int send_request(void)
{
    uint8_t svc[4] = { (uint8_t)(dconn.service >> 24), (uint8_t)(dconn.service >> 16),
                       (uint8_t)(dconn.service >> 8), (uint8_t)dconn.service };
    return dccp_send_pkt(DCCP_PKT_REQUEST, 0, 0, svc, 4, 0, 0);
}

void dccp_handle(ip_hdr_t *ip, void *pkt, int len);

/* One received Ethernet frame, bounded by the IP total length (Ethernet pads
 * short frames; the checksum must not cover the padding) and sanity-checked. */
static void dccp_rx_frame(uint8_t *frame, int len)
{
    if (len < 14 + (int)sizeof(ip_hdr_t)) return;
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    if (ntohs(eth->type) == ETHERTYPE_ARP) { arp_handle(frame, len); return; }
    if (ntohs(eth->type) != ETHERTYPE_IP) return;
    uint8_t *ipd = frame + 14;
    int iplen = len - 14;
    ip_hdr_t *ip = (ip_hdr_t *)ipd;
    if ((ip->ver_ihl >> 4) != 4) return;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    int total = ntohs(ip->total_len);
    if (ihl < 20 || total < ihl + 12 || total > iplen) return;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < ihl; i += 2) sum += (uint32_t)((ipd[i] << 8) | ipd[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    if (sum != 0xFFFF) return;
    if (ip->dst_ip != net_ip || ip->protocol != IPPROTO_DCCP) return;
    dccp_handle(ip, ipd + ihl, total - ihl);
}

/* -----------------------------------------------------------------------
 * Public API — connect (blocking handshake)
 * ----------------------------------------------------------------------- */
int dccp_connect_service(uint32_t dst_ip, uint16_t dst_port, uint32_t service_code) {
    if (dconn_active) return -1;
    memset(&dconn, 0, sizeof(dconn));
    dconn.dst_ip   = dst_ip;
    dconn.dst_port = dst_port;
    dconn.service  = service_code;
    /* random port and initial sequence number (they were 49300 and 1) */
    dconn.src_port = (uint16_t)(49152 + csprng_u32() % 16384);
    {
        uint8_t r[6];
        csprng_fill(r, 6);
        dconn.iss = get48(r);
    }
    dconn.gss = (dconn.iss - 1) & DCCP_SEQ_MASK;     /* the first packet takes iss */
    dconn.state = DCCP_STATE_REQUEST;
    dconn_active = 1;

    if (send_request() != 0) { dconn_active = 0; return -1; }

    /* Wall-clock handshake with a Request retransmission every second (the old
     * loop counted 400 polls and sent the Request exactly once). */
    uint32_t start = debugmon_uptime_ms();
    uint32_t deadline = start + 5000, next_retx = start + 1000;
    while (debugmon_uptime_ms() < deadline) {
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) dccp_rx_frame(pkt, plen);
        if (dconn.state == DCCP_STATE_OPEN) return 0;
        if (!dconn_active) return -1;                /* reset by the peer */
        if (debugmon_uptime_ms() >= next_retx) {
            next_retx += 1000;
            if (send_request() != 0) break;
        }
    }
    if (dconn.rx_buf) { free(dconn.rx_buf); dconn.rx_buf = 0; }
    dconn_active = 0;
    return -1;
}

int dccp_connect(uint32_t dst_ip, uint16_t dst_port) {
    return dccp_connect_service(dst_ip, dst_port, 0);
}

/* -----------------------------------------------------------------------
 * Public API — send data (DCCP-Data, no acknowledgement number)
 * ----------------------------------------------------------------------- */
int dccp_send(const void *data, int len) {
    if (!dconn_active || dconn.state != DCCP_STATE_OPEN) return -1;
    if (len < 0 || len > IP_MAX_PAYLOAD - 16) return -1;
    return dccp_send_pkt(DCCP_PKT_DATA, 0, 0, 0, 0, data, len);
}

/* -----------------------------------------------------------------------
 * Public API — recv (blocking poll, 10 s of silence ends it)
 * ----------------------------------------------------------------------- */
int dccp_recv(uint8_t *buf, int max_len) {
    if (!dconn_active) return -1;
    uint32_t deadline = debugmon_uptime_ms() + 10000;
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
        if (debugmon_uptime_ms() >= deadline) return DCCP_ERR_TIMEOUT;
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) { deadline = debugmon_uptime_ms() + 10000; dccp_rx_frame(pkt, plen); }
    }
}

/* -----------------------------------------------------------------------
 * Public API — close: Close, then wait (briefly) for the peer's Reset
 * ----------------------------------------------------------------------- */
void dccp_close(void) {
    if (!dconn_active) return;
    if (dconn.state == DCCP_STATE_OPEN && dconn.have_gsr) {
        dccp_send_pkt(DCCP_PKT_CLOSE, 1, dconn.gsr, 0, 0, 0, 0);
        dconn.state = DCCP_STATE_CLOSING;
        uint32_t deadline = debugmon_uptime_ms() + 1000;
        while (dconn.state == DCCP_STATE_CLOSING && debugmon_uptime_ms() < deadline) {
            uint8_t pkt[1536];
            int plen = nic_poll(pkt, sizeof(pkt));
            if (plen > 0) dccp_rx_frame(pkt, plen);
        }
    }
    if (dconn.rx_buf) { free(dconn.rx_buf); dconn.rx_buf = 0; }
    dconn.rx_len = 0;
    dconn.state = DCCP_STATE_CLOSED;
    dconn_active = 0;
}

/* -----------------------------------------------------------------------
 * Incoming packet handler (RFC 4340 sections 5, 7.5, 8)
 * ----------------------------------------------------------------------- */
/* bytes needed after the generic header, per packet type */
static int type_header_len(int type)
{
    switch (type) {
    case DCCP_PKT_REQUEST:  return 4;                    /* service code */
    case DCCP_PKT_RESPONSE: return 8 + 4;                /* ack subheader + service code */
    case DCCP_PKT_DATA:     return 0;
    case DCCP_PKT_ACK:
    case DCCP_PKT_DATAACK:
    case DCCP_PKT_CLOSEREQ:
    case DCCP_PKT_CLOSE:
    case DCCP_PKT_SYNC:
    case DCCP_PKT_SYNCACK:  return 8;                    /* ack subheader */
    case DCCP_PKT_RESET:    return 8 + 4;                /* ack subheader + reset code and data */
    default:                return -1;                   /* reserved type */
    }
}

void dccp_handle(ip_hdr_t *ip, void *pkt, int len) {
    if (!dconn_active) return;
    if (len < 12) return;

    uint8_t *h = (uint8_t *)pkt;
    /* The packet's source is the peer's port, its destination ours. (The old
     * check compared them the other way round, so nothing a real peer sent could
     * ever match.) */
    if (get16(h + 0) != dconn.dst_port || get16(h + 2) != dconn.src_port) return;
    if (ip->src_ip != dconn.dst_ip) return;

    int x = h[8] & 1;
    int type = (h[8] >> 1) & 0x0F;
    int gen_len = x ? 16 : 12;
    int doff = h[4] * 4;
    if (len < gen_len) return;
    if ((h[5] & 0x0F) != 0) return;                      /* partial checksum coverage: not supported */
    int tl = type_header_len(type);
    if (tl < 0) return;
    if (doff < gen_len + tl || doff > len) return;       /* header must hold what the type needs, and fit */
    if (!x && type != DCCP_PKT_DATA && type != DCCP_PKT_ACK && type != DCCP_PKT_DATAACK) return;  /* short seqnos: only these */

    /* checksum over the whole packet */
    {
        uint8_t *copy = (uint8_t *)malloc((size_t)len);
        if (!copy) return;
        memcpy(copy, h, (size_t)len);
        uint16_t got = get16(copy + 6);
        put16(copy + 6, 0);
        uint16_t want = dccp_checksum(ip->src_ip, ip->dst_ip, copy, len);
        free(copy);
        if (want != got) return;
    }

    /* sequence number (extend a 24-bit one around GSR) */
    uint64_t seq;
    if (x) seq = get48(h + 10);
    else {
        uint32_t s24 = ((uint32_t)h[9] << 16) | ((uint32_t)h[10] << 8) | h[11];
        seq = (dconn.gsr & ~0xFFFFFFULL) | s24;
        int64_t d = seq_diff(seq, dconn.gsr);
        if (d > (1 << 23)) seq = seq_add(seq, DCCP_SEQ_MASK + 1 - (1 << 24));
        else if (d < -(1 << 23)) seq = seq_add(seq, 1 << 24);
    }
    int has_ack = (type_header_len(type) >= 8) && type != DCCP_PKT_REQUEST;
    uint64_t ack = has_ack ? get48(h + gen_len + 2) : 0;

    /* Acknowledgement number validity: it must acknowledge something we sent,
     * inside the sequence window (RFC 4340 7.5.1). */
    if (has_ack) {
        int64_t back = seq_diff(dconn.gss, ack);
        if (back < 0 || back >= DCCP_SEQ_WINDOW) return;
    }

    switch (dconn.state) {
    case DCCP_STATE_REQUEST:
        /* only a Response that acknowledges one of our Requests is acceptable */
        if (type == DCCP_PKT_RESPONSE) {
            if (seq_diff(ack, dconn.iss) < 0) return;
            dconn.gsr = seq; dconn.have_gsr = 1;
            dccp_send_pkt(DCCP_PKT_ACK, 1, dconn.gsr, 0, 0, 0, 0);
            dconn.state = DCCP_STATE_OPEN;
        } else if (type == DCCP_PKT_RESET) {
            dconn.rx_closed = 1;
            dconn.state = DCCP_STATE_CLOSED;
            dconn_active = 0;
        }
        return;
    default:
        break;
    }

    /* Sequence validity window: SWL = GSR + 1 - W/4, SWH = GSR + 3W/4. */
    int64_t d = seq_diff(seq, dconn.gsr);
    if (d < -(DCCP_SEQ_WINDOW / 4) + 1 || d > 3 * DCCP_SEQ_WINDOW / 4) return;
    if (d > 0) dconn.gsr = seq;

    int dlen = len - doff;
    uint8_t *data = h + doff;

    switch (type) {
    case DCCP_PKT_DATA:
    case DCCP_PKT_DATAACK:
        if (dconn.state == DCCP_STATE_OPEN && dlen > 0 && dconn.rx_len + dlen <= DCCP_RX_MAX) {
            uint8_t *tmp = (uint8_t *)malloc((size_t)(dconn.rx_len + dlen));
            if (tmp) {
                if (dconn.rx_len > 0) memcpy(tmp, dconn.rx_buf, (size_t)dconn.rx_len);
                memcpy(tmp + dconn.rx_len, data, (size_t)dlen);
                free(dconn.rx_buf);
                dconn.rx_buf = tmp;
                dconn.rx_len += dlen;
            }
        }
        break;

    case DCCP_PKT_CLOSEREQ:
        /* The server asks us to close: answer with Close and wait for its Reset. */
        if (dconn.state == DCCP_STATE_OPEN) {
            dccp_send_pkt(DCCP_PKT_CLOSE, 1, dconn.gsr, 0, 0, 0, 0);
            dconn.state = DCCP_STATE_CLOSING;
            dconn.rx_closed = 1;
        }
        break;

    case DCCP_PKT_CLOSE: {
        /* Answer a Close with a Reset (code 'Closed'); never answer a Reset. */
        uint8_t rc[4] = { DCCP_RESET_CLOSED, 0, 0, 0 };
        dccp_send_pkt(DCCP_PKT_RESET, 1, dconn.gsr, rc, 4, 0, 0);
        dconn.rx_closed = 1;
        dconn.state = DCCP_STATE_CLOSED;
        break;
    }

    case DCCP_PKT_RESET:
        dconn.rx_closed = 1;
        dconn.state     = DCCP_STATE_CLOSED;
        break;

    case DCCP_PKT_SYNC:
        /* RFC 4340 7.5.4: a Sync must be answered with a SyncAck acknowledging it */
        dccp_send_pkt(DCCP_PKT_SYNCACK, 1, seq, 0, 0, 0, 0);
        break;

    default:
        break;
    }
}
