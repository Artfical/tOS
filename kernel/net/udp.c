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

int udp_open(uint16_t port)
{
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
    if (seg_len & 1) sum += ((uint8_t *)udp_seg)[seg_len - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(sum == 0 ? 0xFFFF : ~sum & 0xFFFF);
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, void *data, int len)
{
    int total = sizeof(udp_hdr_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

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

void udp_handle(ip_hdr_t *ip, void *pkt, int len)
{
    if (len < (int)sizeof(udp_hdr_t)) return;
    udp_hdr_t *udp = (udp_hdr_t *)pkt;
    int data_len = len - sizeof(udp_hdr_t);
    uint16_t dst_port = ntohs(udp->dst_port);

    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (udp_sockets[i].used && udp_sockets[i].port == dst_port) {
            udp_sockets[i].data = (uint8_t *)malloc(data_len);
            if (udp_sockets[i].data) {
                memcpy(udp_sockets[i].data, (uint8_t *)pkt + sizeof(udp_hdr_t), data_len);
                udp_sockets[i].len = data_len;
                udp_sockets[i].has_data = 1;
                udp_sockets[i].src_ip = ip->src_ip;
                udp_sockets[i].src_port = ntohs(udp->src_port);
            }
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
