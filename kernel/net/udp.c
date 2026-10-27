#include "udp.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "wgtun.h"

typedef struct {
    int      used;
    uint16_t port;
    int      has_data;
    uint8_t *data;
    int      len;
    uint32_t src_ip;
    uint16_t src_port;
} udp_socket_t;

#define UDP_SOCKETS 16
static udp_socket_t udp_sockets[UDP_SOCKETS];

void udp_init(void)
{
    for (int i = 0; i < UDP_SOCKETS; i++)
        udp_sockets[i].used = 0;
}

int udp_listen(uint16_t port, uint8_t *resp, int max_len, uint32_t *src_ip, uint16_t *src_port)
{
    int idx = -1;
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (!udp_sockets[i].used) continue;
        if (udp_sockets[i].port == port && udp_sockets[i].has_data) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    int n = udp_sockets[idx].len;
    if (n > max_len) n = max_len;
    memcpy(resp, udp_sockets[idx].data, n);
    if (src_ip) *src_ip = udp_sockets[idx].src_ip;
    if (src_port) *src_port = udp_sockets[idx].src_port;
    free(udp_sockets[idx].data);
    udp_sockets[idx].has_data = 0;
    udp_sockets[idx].data = 0;
    return n;
}

/* a datagram waiting on the port? (without taking it) */
int udp_has_data(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKETS; i++)
        if (udp_sockets[i].used && udp_sockets[i].port == port && udp_sockets[i].has_data) return udp_sockets[i].len;
    return -1;
}

int udp_port_open(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKETS; i++)
        if (udp_sockets[i].used && udp_sockets[i].port == port) return 1;
    return 0;
}

void udp_close(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (udp_sockets[i].used && udp_sockets[i].port == port) {
            if (udp_sockets[i].data) free(udp_sockets[i].data);
            udp_sockets[i].data = 0;
            udp_sockets[i].has_data = 0;
            udp_sockets[i].used = 0;
        }
    }
}

int udp_open(uint16_t port)
{
    /* There is no udp_close(), so callers that open the same port again
     * (every dns_resolve() does) used to burn one of the 4 sockets each
     * time until none were left. Reuse the existing one. */
    for (int i = 0; i < UDP_SOCKETS; i++)
        if (udp_sockets[i].used && udp_sockets[i].port == port) return i;
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (!udp_sockets[i].used) {
            udp_sockets[i].used = 1;
            udp_sockets[i].port = port;
            udp_sockets[i].has_data = 0;
            udp_sockets[i].data = 0;
            return i;
        }
    }
    return -1;
}

/* The UDP checksum over the IPv4 pseudo-header (addresses in network order as they sit in the
 * packet) and the segment. Everything is read a byte at a time: the pseudo-header used to be a
 * struct read through a uint16_t pointer, and GCC 13 (the one CI builds releases with) was
 * entitled to drop the address stores before that read -- every DNS query from a released
 * kernel went out with a checksum made from zero addresses and was thrown away. */
static uint16_t udp_checksum(uint32_t src, uint32_t dst, const void *udp_seg, int seg_len)
{
    uint8_t ph[12];
    memcpy(ph, &src, 4);
    memcpy(ph + 4, &dst, 4);
    ph[8] = 0;
    ph[9] = IPPROTO_UDP;
    ph[10] = (uint8_t)(seg_len >> 8);
    ph[11] = (uint8_t)seg_len;
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)ph[i] << 8 | ph[i + 1];
    const uint8_t *p = (const uint8_t *)udp_seg;
    for (int i = 0; i + 1 < seg_len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    /* a trailing odd byte is padded with an implicit zero low byte */
    if (seg_len & 1) sum += (uint32_t)p[seg_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(sum == 0 ? 0xFFFF : ~sum & 0xFFFF);
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, void *data, int len)
{
    if (len < 0 || len > IP_MAX_PAYLOAD - (int)sizeof(udp_hdr_t)) return IP_ERR_TOOBIG;
    int total = sizeof(udp_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return IP_ERR_NOMEM; /* same class of failure as ip_send()'s own OOM check */

    udp_hdr_t *udp = (udp_hdr_t *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(total);
    udp->checksum = 0;
    memcpy(buf + sizeof(udp_hdr_t), data, len);

    udp->checksum = udp_checksum(net_ip, dst_ip, buf, total);

    int r = ip_send(dst_ip, IPPROTO_UDP, buf, total);
    free(buf);
    return r;
}

#define UDP_MAX_DATAGRAM 65507   /* 65535 - 20 (IPv4) - 8 (UDP) */

void udp_handle(ip_hdr_t *ip, void *pkt, int len)
{
    if (len < (int)sizeof(udp_hdr_t)) return;
    udp_hdr_t *udp = (udp_hdr_t *)pkt;
    /* The UDP length field is attacker-controlled: it must cover at least
     * the header and not claim more than the IP layer delivered. Anything
     * past it (e.g. Ethernet padding) is not payload. */
    int ulen = ntohs(udp->length);
    if (ulen < (int)sizeof(udp_hdr_t) || ulen > len) return;
    int data_len = ulen - (int)sizeof(udp_hdr_t);
    if (data_len > UDP_MAX_DATAGRAM) return;
    uint16_t dst_port = ntohs(udp->dst_port);

    /* A non-zero UDP checksum is verified (0 means the sender did not compute one,
     * which IPv4 allows). It was never checked, so datagrams damaged in transit
     * were handed to DNS, the tunnel and applications as if intact. */
    if (udp->checksum != 0) {
        uint8_t *copy = (uint8_t *)malloc((size_t)ulen);
        if (!copy) return;
        memcpy(copy, pkt, (size_t)ulen);
        ((udp_hdr_t *)copy)->checksum = 0;
        uint16_t want = udp_checksum(ip->src_ip, ip->dst_ip, copy, ulen);
        free(copy);
        if (want != udp->checksum) return;
    }

    /* datagrams for an encrypted tunnel's local port belong to the tunnel (they
     * were never handed to wgtun_rx(): the tunnel could not receive anything) */
    if (wgtun_owns_port(dst_port)) {
        wgtun_rx(ip->src_ip, ntohs(udp->src_port), (const uint8_t *)pkt + sizeof(udp_hdr_t), data_len);
        return;
    }

    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (udp_sockets[i].used && udp_sockets[i].port == dst_port) {
            /* One queued datagram per socket: a second one used to
             * overwrite .data without freeing it, leaking a buffer per
             * packet for as long as the sender kept flooding. Drop it. */
            if (udp_sockets[i].has_data) return;
            uint8_t *copy = (uint8_t *)malloc(data_len > 0 ? (size_t)data_len : 1);
            if (!copy) return;
            memcpy(copy, (uint8_t *)pkt + sizeof(udp_hdr_t), (size_t)data_len);
            udp_sockets[i].data = copy;
            udp_sockets[i].len = data_len;
            udp_sockets[i].has_data = 1;
            udp_sockets[i].src_ip = ip->src_ip;
            udp_sockets[i].src_port = ntohs(udp->src_port);
            return;
        }
    }
}

/* Read-only query API for Network Monitor */
int udp_get_sockets(udp_sock_info_t *out, int max)
{
    int n = 0, i;
    for (i = 0; i < UDP_SOCKETS && n < max; i++) {
        if (!udp_sockets[i].used) continue;
        out[n].port     = udp_sockets[i].port;
        out[n].has_data = udp_sockets[i].has_data;
        n++;
    }
    return n;
}
