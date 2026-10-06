#include "udplite.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "debugmon.h"

/* -----------------------------------------------------------------------
 * Socket table (mirrors UDP's pattern)
 * ----------------------------------------------------------------------- */
typedef struct {
    int      used;
    uint16_t port;
    int      has_data;
    uint8_t *data;
    int      len;
    uint32_t src_ip;
    uint16_t src_port;
} udplite_socket_t;

static udplite_socket_t sockets[UDPLITE_SOCKETS];

/* -----------------------------------------------------------------------
 * Checksum — same pseudo-header as UDP (RFC 3828 §3.1)
 * coverage=0 means entire datagram; minimum = 8 (header only)
 * ----------------------------------------------------------------------- */
static uint16_t udplite_checksum(uint32_t src_ip, uint32_t dst_ip,
                                  const uint8_t *seg, int seg_len,
                                  uint16_t coverage) {
    int check_len = (coverage == 0 || coverage > seg_len) ? seg_len : coverage;

    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;   /* always full segment length in pseudo-header */
    } __attribute__((packed)) pseudo;
    pseudo.src   = src_ip;
    pseudo.dst   = dst_ip;
    pseudo.zero  = 0;
    pseudo.proto = IPPROTO_UDPLITE;
    pseudo.len   = htons((uint16_t)seg_len);

    uint32_t sum = 0;
    const uint8_t *pb = (const uint8_t *)&pseudo;
    for (int i = 0; i + 1 < (int)sizeof(pseudo); i += 2) sum += (uint32_t)((pb[i] << 8) | pb[i + 1]);
    for (int i = 0; i + 1 < check_len; i += 2) sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    if (check_len & 1) sum += (uint16_t)seg[check_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(sum == 0 ? 0xFFFF : (~sum & 0xFFFF));
}

/* -----------------------------------------------------------------------
 * Public API — open a receive socket
 * ----------------------------------------------------------------------- */
int udplite_open(uint16_t port) {
    for (int i = 0; i < UDPLITE_SOCKETS; i++)          /* opening a port twice used to take a second slot */
        if (sockets[i].used && sockets[i].port == port) return 0;
    for (int i = 0; i < UDPLITE_SOCKETS; i++) {
        if (!sockets[i].used) {
            sockets[i].used     = 1;
            sockets[i].port     = port;
            sockets[i].has_data = 0;
            sockets[i].data     = 0;
            sockets[i].len      = 0;
            return 0;
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API — send
 * ----------------------------------------------------------------------- */
int udplite_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                 void *data, int len, uint16_t coverage) {
    /* A negative length made 'total' smaller than the header (the 8-byte header
     * was then written into a 7-byte buffer: heap overflow) and a large one went
     * out as an oversized frame; ip_send() refuses anything above the MTU. */
    if (len < 0 || len > IP_MAX_PAYLOAD - (int)sizeof(udplite_hdr_t)) return IP_ERR_TOOBIG;
    int total = sizeof(udplite_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    udplite_hdr_t *h  = (udplite_hdr_t *)buf;
    h->src_port          = htons(src_port);
    h->dst_port          = htons(dst_port);
    /* coverage: 0 = full datagram; values 1-7 are illegal per RFC 3828 §3.1,
       so normalise those to 8 (header only), and a value beyond the datagram
       (also illegal: receivers drop it) to the datagram length. */
    if (coverage > 0 && coverage < 8) coverage = 8;
    if (coverage > total) coverage = (uint16_t)total;
    h->checksum_coverage  = htons(coverage);
    h->checksum           = 0;
    memcpy(buf + sizeof(udplite_hdr_t), data, len);

    h->checksum = udplite_checksum(net_ip, dst_ip, buf, total, coverage);

    int r = ip_send(dst_ip, IPPROTO_UDPLITE, buf, total);
    free(buf);
    return r;
}

/* -----------------------------------------------------------------------
 * Public API — blocking listen/recv (like udp_listen)
 * ----------------------------------------------------------------------- */
int udplite_listen(uint16_t port, uint8_t *buf, int max_len,
                   uint32_t *src_ip, uint16_t *src_port) {
    int slot = -1;
    for (int i = 0; i < UDPLITE_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].port == port) { slot = i; break; }
    }
    if (slot < 0 && udplite_open(port) == 0) {
        for (int i = 0; i < UDPLITE_SOCKETS; i++)
            if (sockets[i].used && sockets[i].port == port) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* wall-clock timeout (5 s): the old '2000 polls' lasted however long the NIC driver took */
    uint32_t deadline = debugmon_uptime_ms() + 5000;
    while (debugmon_uptime_ms() < deadline) {
        if (sockets[slot].has_data) {
            int n = sockets[slot].len < max_len ? sockets[slot].len : max_len;
            memcpy(buf, sockets[slot].data, n);
            if (src_ip)   *src_ip   = sockets[slot].src_ip;
            if (src_port) *src_port = sockets[slot].src_port;
            free(sockets[slot].data);
            sockets[slot].data     = 0;
            sockets[slot].has_data = 0;
            sockets[slot].len      = 0;
            return n;
        }
        /* need to poll — caller should call net_poll() in a loop, but
           for simplicity mirror the UDP blocking pattern */
        extern int (*nic_poll)(uint8_t *buf, int max_len);
        if (!nic_poll) break;
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            extern void arp_handle(uint8_t *, int);
            extern void ip_handle(uint8_t *, int);
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(pkt, plen);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(pkt + sizeof(eth_hdr_t), plen - sizeof(eth_hdr_t));
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Incoming handler (called from ip_handle)
 * ----------------------------------------------------------------------- */
void udplite_handle(ip_hdr_t *ip, void *pkt, int len) {
    if (len < (int)sizeof(udplite_hdr_t)) return;
    udplite_hdr_t *h = (udplite_hdr_t *)pkt;
    uint16_t dst_port = ntohs(h->dst_port);
    int data_len      = len - sizeof(udplite_hdr_t);

    /* RFC 3828 3.1/3.2: the checksum is mandatory (0 is illegal), coverage is 0
     * (whole datagram) or 8..length. Neither was checked, so corrupted datagrams
     * were accepted. Bytes beyond the coverage may be damaged: that is the point
     * of UDP-Lite, and it is why the sum covers only the first 'coverage' bytes. */
    uint16_t cov = ntohs(h->checksum_coverage);
    if (cov != 0 && (cov < 8 || cov > len)) return;
    if (h->checksum == 0) return;
    {
        uint8_t *copy = (uint8_t *)malloc((size_t)len);
        if (!copy) return;
        memcpy(copy, pkt, (size_t)len);
        ((udplite_hdr_t *)copy)->checksum = 0;
        uint16_t want = udplite_checksum(ip->src_ip, ip->dst_ip, copy, len, cov == (uint16_t)len ? 0 : cov);
        free(copy);
        if (want != h->checksum) return;
    }

    for (int i = 0; i < UDPLITE_SOCKETS; i++) {
        if (!sockets[i].used || sockets[i].port != dst_port) continue;
        /* Allocate first, then replace: freeing the queued datagram before the
         * allocation left has_data set with a NULL (or freed) buffer when malloc
         * failed, and the next listen() copied from it. */
        uint8_t *copy = (uint8_t *)malloc(data_len > 0 ? (size_t)data_len : 1);
        if (!copy) return;
        if (data_len > 0) memcpy(copy, (uint8_t *)pkt + sizeof(udplite_hdr_t), (size_t)data_len);
        if (sockets[i].data) free(sockets[i].data);
        sockets[i].data     = copy;
        sockets[i].len      = data_len;
        sockets[i].has_data = 1;
        sockets[i].src_ip   = ip->src_ip;
        sockets[i].src_port = ntohs(h->src_port);
        return;
    }
}
