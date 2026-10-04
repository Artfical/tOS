#include "udp.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "memory.h"

typedef struct {
    int      used;
    uint16_t port;
    int      has_data;
    uint8_t *data;
    int      len;
    uint32_t src_ip;
    uint16_t src_port;
} udp_socket_t;

#define UDP_SOCKETS 4
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

static uint16_t udp_checksum(void *pseudo, int pseudo_len, void *udp_seg, int seg_len)
{
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)pseudo;
    for (int i = 0; i < pseudo_len / 2; i++) sum += ntohs(p[i]);
    p = (uint16_t *)udp_seg;
    for (int i = 0; i < seg_len / 2; i++) sum += ntohs(p[i]);
    /* A trailing odd byte is padded with an implicit zero LOW byte, i.e.
     * it occupies the HIGH byte of the final 16-bit word — not a plain
     * byte value. DNS queries (variable-length hostnames) hit this path
     * far more often than ICMP's fixed-size echo payloads, which is why
     * this was silently corrupting almost every DNS query checksum. */
    if (seg_len & 1) sum += (uint32_t)((uint8_t *)udp_seg)[seg_len - 1] << 8;
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

    struct { uint32_t src; uint32_t dst; uint8_t zero; uint8_t proto; uint16_t len; } __attribute__((packed)) pseudo;
    pseudo.src = net_ip;
    pseudo.dst = dst_ip;
    pseudo.zero = 0;
    pseudo.proto = IPPROTO_UDP;
    pseudo.len = htons(total);
    udp->checksum = udp_checksum(&pseudo, sizeof(pseudo), buf, total);

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
