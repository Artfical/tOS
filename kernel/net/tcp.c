#include "tcp.h"
#include "arp.h"
#include "net.h"
#include "rtl8139.h"
#include "string.h"
#include "memory.h"

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_SENT    3
#define TCP_LAST_ACK    4

static struct {
    int      state;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t *rx_buf;
    int      rx_len;
    int      rx_cap;
    int      rx_closed;
    uint16_t ip_id;
} conn;

static int conn_active = 0;
static uint16_t tcp_ip_id = 0;

static int send_seg(uint8_t flags, void *payload, int payload_len)
{
    if (!conn_active) return -1;
    uint8_t mac[6];
    if (arp_resolve(conn.dst_ip, mac) != 0) return -1;

    int tcp_len = 20 + payload_len;
    int total = 14 + 20 + tcp_len;
    uint8_t *pkt = (uint8_t *)malloc(total);
    if (!pkt) return -1;

    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(pkt + 14);
    memset(ip, 0, sizeof(ip_hdr_t));
    ip->ver_ihl = 0x45;
    ip->total_len = htons(20 + tcp_len);
    ip->id = htons(tcp_ip_id);
    tcp_ip_id++;
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = net_ip;
    ip->dst_ip = conn.dst_ip;

    uint8_t *tcp = pkt + 14 + 20;
    memset(tcp, 0, 20);
    *(uint16_t *)(tcp + 0) = htons(conn.src_port);
    *(uint16_t *)(tcp + 2) = htons(conn.dst_port);
    *(uint32_t *)(tcp + 4) = htonl(conn.seq);
    *(uint32_t *)(tcp + 8) = htonl(conn.ack);
    *(tcp + 12) = 0x50;
    *(tcp + 13) = flags;
    *(uint16_t *)(tcp + 14) = htons(65535);
    if (payload_len > 0) memcpy(tcp + 20, payload, payload_len);

    uint8_t pseudo[12];
    *(uint32_t *)(pseudo + 0) = net_ip;
    *(uint32_t *)(pseudo + 4) = conn.dst_ip;
    *(pseudo + 8) = 0;
    *(pseudo + 9) = IPPROTO_TCP;
    *(uint16_t *)(pseudo + 10) = htons(tcp_len);

    uint32_t sum = 0;
    for (int i = 0; i < 6; i++) sum += ntohs(((uint16_t *)pseudo)[i]);
    for (int i = 0; i < (tcp_len + 1) / 2; i++) sum += ntohs(((uint16_t *)tcp)[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    *(uint16_t *)(tcp + 16) = htons(~sum & 0xFFFF);

    ip->checksum = 0;
    sum = 0;
    for (int i = 0; i < 10; i++) sum += ntohs(((uint16_t *)ip)[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->checksum = htons(~sum & 0xFFFF);

    rtl8139_send(pkt, total);
    free(pkt);

    if (!(flags & TCP_FLAG_SYN)) conn.seq += payload_len;
    if (flags & TCP_FLAG_SYN) conn.seq++;
    return 0;
}

int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    conn.state = TCP_CLOSED;
    conn.dst_ip = dst_ip;
    conn.dst_port = dst_port;
    conn.src_port = 49152 + (dst_port & 0x3FFF);
    conn.seq = 0x1000;
    conn.ack = 0;
    conn.rx_buf = 0;
    conn.rx_len = 0;
    conn.rx_cap = 0;
    conn.rx_closed = 0;
    conn.ip_id = 0;
    conn_active = 1;

    conn.state = TCP_SYN_SENT;
    if (send_seg(TCP_FLAG_SYN, 0, 0) != 0) { conn_active = 0; return -1; }

    for (int retry = 0; retry < 200; retry++) {
        uint8_t buf[1536];
        int len = rtl8139_poll(buf, sizeof(buf));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(buf, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
        if (conn.state == TCP_ESTABLISHED) return 0;
    }
    conn_active = 0;
    return -1;
}

int tcp_send(void *data, int len)
{
    if (!conn_active || conn.state != TCP_ESTABLISHED) return -1;
    if (send_seg(TCP_FLAG_PSH | TCP_FLAG_ACK, data, len) != 0) return -1;
    return 0;
}

int tcp_recv(uint8_t *buf, int max_len)
{
    if (!conn_active) return -1;
    for (;;) {
        if (conn.rx_len > 0) {
            int n = conn.rx_len;
            if (n > max_len) n = max_len;
            memcpy(buf, conn.rx_buf, n);
            conn.rx_len -= n;
            if (conn.rx_len > 0)
                memmove(conn.rx_buf, conn.rx_buf + n, conn.rx_len);
            else {
                free(conn.rx_buf);
                conn.rx_buf = 0;
                conn.rx_cap = 0;
            }
            return n;
        }
        if (conn.rx_closed) return 0;
        uint8_t pkt[1536];
        int len = rtl8139_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(pkt, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(pkt + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
    }
}

void tcp_close(void)
{
    if (!conn_active) return;
    if (conn.state == TCP_ESTABLISHED) {
        conn.state = TCP_FIN_SENT;
        send_seg(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        for (int retry = 0; retry < 50000; retry++) {
            uint8_t pkt[1536];
            int len = rtl8139_poll(pkt, sizeof(pkt));
            if (len > 0) {
                eth_hdr_t *eth = (eth_hdr_t *)pkt;
                if (ntohs(eth->type) == ETHERTYPE_ARP) arp_handle(pkt, len);
                else if (ntohs(eth->type) == ETHERTYPE_IP)
                    ip_handle(pkt + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
            }
            if (conn.state == TCP_CLOSED) break;
        }
    }
    if (conn.rx_buf) free(conn.rx_buf);
    conn_active = 0;
}

void tcp_handle(ip_hdr_t *ip, void *pkt, int len)
{
    (void)ip;
    if (!conn_active) return;
    if (len < 20) return;

    uint16_t src_port = ntohs(*(uint16_t *)pkt);
    uint16_t dst_port = ntohs(*(uint16_t *)((uint8_t *)pkt + 2));
    (void)dst_port;

    if (src_port != conn.dst_port || ntohs(*(uint16_t *)((uint8_t *)pkt + 2)) != conn.src_port)
        return;

    uint32_t pkt_seq = ntohl(*(uint32_t *)((uint8_t *)pkt + 4));
    uint32_t pkt_ack = ntohl(*(uint32_t *)((uint8_t *)pkt + 8));
    uint8_t data_off = (*(uint8_t *)((uint8_t *)pkt + 12)) >> 4;
    uint8_t flags = *(uint8_t *)((uint8_t *)pkt + 13);
    int hdr_len = data_off * 4;
    int data_len = len - hdr_len;

    if (flags & TCP_FLAG_RST) {
        conn.state = TCP_CLOSED;
        return;
    }

    if (conn.state == TCP_SYN_SENT && (flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
        conn.ack = pkt_seq + 1;
        conn.seq = pkt_ack;
        conn.state = TCP_ESTABLISHED;
        send_seg(TCP_FLAG_ACK, 0, 0);
        return;
    }

    if (conn.state != TCP_ESTABLISHED && conn.state != TCP_FIN_SENT) return;

    if (pkt_seq != conn.ack) return;

    if (data_len > 0) {
        if (conn.rx_buf == 0) {
            conn.rx_buf = (uint8_t *)malloc(data_len);
            if (conn.rx_buf) { memcpy(conn.rx_buf, (uint8_t *)pkt + hdr_len, data_len); conn.rx_len = data_len; conn.rx_cap = data_len; }
        } else {
            uint8_t *tmp = (uint8_t *)krealloc(conn.rx_buf, conn.rx_len + data_len);
            if (tmp) { memcpy(tmp + conn.rx_len, (uint8_t *)pkt + hdr_len, data_len); conn.rx_buf = tmp; conn.rx_len += data_len; conn.rx_cap = conn.rx_len; }
        }
        conn.ack += data_len;
        send_seg(TCP_FLAG_ACK, 0, 0);
    }

    if (flags & TCP_FLAG_FIN) {
        conn.ack++;
        conn.rx_closed = 1;
        if (conn.state == TCP_FIN_SENT) {
            conn.state = TCP_CLOSED;
        } else {
            conn.state = TCP_LAST_ACK;
            send_seg(TCP_FLAG_ACK, 0, 0);
        }
    }

    if (flags & TCP_FLAG_ACK) {
        if (conn.state == TCP_FIN_SENT) conn.state = TCP_CLOSED;
    }
}
