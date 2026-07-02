/*
 * tcp.c - Multi-connection TCP for tOS
 *
 * Up to TCP_MAX_SOCKETS (16) simultaneous connections.
 * Full RFC 793 state machine: LISTEN, SYN_SENT, SYN_RECEIVED,
 * ESTABLISHED, FIN_WAIT_1/2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT.
 * Basic slow-start congestion control + retransmission timer per socket.
 *
 * Old blocking API (tcp_connect/send/recv/close) is preserved unchanged
 * for http.c and modsocket.c compatibility.
 */

#include "tcp.h"
#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "memory.h"

/* -- TCP Control Block ---------------------------------------------------- */
typedef struct {
    int      state;
    int      used;

    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;

    uint32_t seq;
    uint32_t ack;
    uint32_t snd_una;

    uint8_t *rx_buf;
    int      rx_len;
    int      rx_cap;
    int      rx_closed;

    uint8_t *retx_data;
    int      retx_len;
    uint32_t retx_seq;
    int      retx_tick;
    int      retx_count;

    uint32_t cwnd;
    uint32_t ssthresh;

    int      accept_queue[4];
    int      accept_head;
    int      accept_tail;
} tcp_sock_t;

static tcp_sock_t socks[TCP_MAX_SOCKETS];
static uint16_t   next_port = 49152;
static uint16_t   tcp_ip_id = 0;

/* -- Helpers --------------------------------------------------------------- */
static tcp_sock_t *get_sock(int fd)
{
    if (fd < 0 || fd >= TCP_MAX_SOCKETS) return 0;
    if (!socks[fd].used) return 0;
    return &socks[fd];
}

static uint16_t alloc_port(void)
{
    uint16_t p = next_port++;
    if (next_port == 0) next_port = 49152;
    return p;
}

static void rx_append(tcp_sock_t *s, const uint8_t *data, int len)
{
    if (s->rx_buf == 0) {
        s->rx_buf = (uint8_t *)malloc(len);
        if (!s->rx_buf) return;
        memcpy(s->rx_buf, data, len);
        s->rx_len = len;
        s->rx_cap = len;
    } else {
        uint8_t *tmp = (uint8_t *)krealloc(s->rx_buf, s->rx_len + len);
        if (!tmp) return;
        memcpy(tmp + s->rx_len, data, len);
        s->rx_buf = tmp;
        s->rx_len += len;
        s->rx_cap  = s->rx_len;
    }
}

/* -- Packet builder -------------------------------------------------------- */
static int send_seg(tcp_sock_t *s, uint8_t flags, const void *payload, int plen)
{
    uint8_t mac[6];
    if (arp_resolve(s->dst_ip, mac) != 0) return -1;

    int tcp_len = 20 + plen;
    int total   = 14 + 20 + tcp_len;
    uint8_t *pkt = (uint8_t *)malloc(total);
    if (!pkt) return -1;

    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(pkt + 14);
    memset(ip, 0, sizeof(ip_hdr_t));
    ip->ver_ihl    = 0x45;
    ip->total_len  = htons(20 + tcp_len);
    uint16_t _tid = tcp_ip_id++; ip->id = htons(_tid);
    ip->flags_frag = htons(0x4000);
    ip->ttl        = 64;
    ip->protocol   = IPPROTO_TCP;
    ip->src_ip     = net_ip;
    ip->dst_ip     = s->dst_ip;

    uint8_t *tcp = pkt + 14 + 20;
    memset(tcp, 0, 20);
    *(uint16_t *)(tcp + 0)  = htons(s->src_port);
    *(uint16_t *)(tcp + 2)  = htons(s->dst_port);
    *(uint32_t *)(tcp + 4)  = htonl(s->seq);
    *(uint32_t *)(tcp + 8)  = htonl(s->ack);
    *(tcp + 12) = 0x50;
    *(tcp + 13) = flags;
    *(uint16_t *)(tcp + 14) = htons(65535);
    if (plen > 0) memcpy(tcp + 20, payload, plen);

    uint8_t pseudo[12];
    *(uint32_t *)(pseudo + 0)  = net_ip;
    *(uint32_t *)(pseudo + 4)  = s->dst_ip;
    *(pseudo + 8)              = 0;
    *(pseudo + 9)              = IPPROTO_TCP;
    *(uint16_t *)(pseudo + 10) = htons(tcp_len);

    uint32_t sum = 0;
    int i;
    for (i = 0; i < 6; i++)                 sum += ntohs(((uint16_t *)pseudo)[i]);
    for (i = 0; i < (tcp_len + 1) / 2; i++) sum += ntohs(((uint16_t *)tcp)[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    *(uint16_t *)(tcp + 16) = htons((uint16_t)(~sum & 0xFFFF));

    ip->checksum = 0;
    sum = 0;
    for (i = 0; i < 10; i++) sum += ntohs(((uint16_t *)ip)[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->checksum = htons((uint16_t)(~sum & 0xFFFF));

    nic_send(pkt, total);
    free(pkt);

    if (flags & TCP_FLAG_SYN) s->seq++;
    if (flags & TCP_FLAG_FIN) s->seq++;
    s->seq += (uint32_t)plen;
    return 0;
}

static void retx_save(tcp_sock_t *s, const void *data, int len)
{
    if (s->retx_data) { free(s->retx_data); s->retx_data = 0; }
    s->retx_seq = s->seq - (uint32_t)len;
    if (len > 0) {
        s->retx_data = (uint8_t *)malloc(len);
        if (s->retx_data) memcpy(s->retx_data, data, len);
    }
    s->retx_len   = len;
    s->retx_tick  = 0;
    s->retx_count = 0;
}

static void retx_clear(tcp_sock_t *s)
{
    if (s->retx_data) { free(s->retx_data); s->retx_data = 0; }
    s->retx_len   = 0;
    s->retx_tick  = 0;
    s->retx_count = 0;
}

/* -- Socket API ------------------------------------------------------------ */
int tcp_socket(void)
{
    int i;
    for (i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(tcp_sock_t));
            socks[i].used     = 1;
            socks[i].state    = TCP_CLOSED;
            socks[i].cwnd     = TCP_MSS;
            socks[i].ssthresh = 65535;
            return i;
        }
    }
    return -1;
}

int tcp_connect2(int fd, uint32_t dst_ip, uint16_t dst_port)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_CLOSED) return -1;

    s->dst_ip   = dst_ip;
    s->dst_port = dst_port;
    s->src_port = alloc_port();
    s->seq      = 0x1000;
    s->ack      = 0;
    s->state    = TCP_SYN_SENT;

    if (send_seg(s, TCP_FLAG_SYN, 0, 0) != 0) {
        s->state = TCP_CLOSED;
        return -1;
    }

    int retry;
    for (retry = 0; retry < 300; retry++) {
        uint8_t buf[1536];
        int len = nic_poll(buf, sizeof(buf));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(buf, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
        if (s->state == TCP_ESTABLISHED) return 0;
        if (s->state == TCP_CLOSED)      return -1;
    }
    s->state = TCP_CLOSED;
    return -1;
}

int tcp_send2(int fd, void *data, int len)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_ESTABLISHED) return -1;
    uint32_t seq_before = s->seq;
    if (send_seg(s, TCP_FLAG_PSH | TCP_FLAG_ACK, data, len) != 0) return -1;
    s->snd_una = seq_before;
    retx_save(s, data, len);
    return 0;
}

int tcp_recv2(int fd, uint8_t *buf, int max_len)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return -1;
    for (;;) {
        if (s->rx_len > 0) {
            int n = s->rx_len < max_len ? s->rx_len : max_len;
            memcpy(buf, s->rx_buf, n);
            s->rx_len -= n;
            if (s->rx_len > 0)
                memmove(s->rx_buf, s->rx_buf + n, s->rx_len);
            else {
                free(s->rx_buf);
                s->rx_buf = 0;
                s->rx_cap = 0;
            }
            return n;
        }
        if (s->rx_closed)           return 0;
        if (s->state == TCP_CLOSED) return -1;
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(pkt, plen);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(pkt + sizeof(eth_hdr_t), plen - sizeof(eth_hdr_t));
        }
    }
}

void tcp_close2(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return;

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_FIN_WAIT_1;
        send_seg(s, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        int retry;
        for (retry = 0; retry < 50000; retry++) {
            uint8_t pkt[1536];
            int len = nic_poll(pkt, sizeof(pkt));
            if (len > 0) {
                eth_hdr_t *eth = (eth_hdr_t *)pkt;
                if (ntohs(eth->type) == ETHERTYPE_ARP) arp_handle(pkt, len);
                else if (ntohs(eth->type) == ETHERTYPE_IP)
                    ip_handle(pkt + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
            }
            if (s->state == TCP_CLOSED || s->state == TCP_TIME_WAIT) break;
        }
    }

    retx_clear(s);
    if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }
    memset(s, 0, sizeof(tcp_sock_t));
}

int tcp_listen(int fd, uint16_t port)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_CLOSED) return -1;
    s->src_port    = port;
    s->dst_ip      = 0;
    s->dst_port    = 0;
    s->state       = TCP_LISTEN;
    s->accept_head = s->accept_tail = 0;
    return 0;
}

int tcp_accept(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_LISTEN) return -1;
    for (;;) {
        if (s->accept_head != s->accept_tail) {
            int new_fd = s->accept_queue[s->accept_head % 4];
            s->accept_head++;
            return new_fd;
        }
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP) arp_handle(pkt, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(pkt + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
    }
}

/* -- Retransmission tick (call every ~100 ms) ------------------------------ */
void tcp_tick(void)
{
    int i;
    for (i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_sock_t *s = &socks[i];
        if (!s->used || s->retx_len == 0 || s->state != TCP_ESTABLISHED)
            continue;

        s->retx_tick++;
        if (s->retx_tick < TCP_RETX_TIMEOUT) continue;
        s->retx_tick = 0;
        s->retx_count++;

        if (s->retx_count > TCP_RETX_MAX) {
            send_seg(s, TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0);
            retx_clear(s);
            s->state = TCP_CLOSED;
            continue;
        }

        s->ssthresh = s->cwnd / 2;
        if (s->ssthresh < (uint32_t)TCP_MSS) s->ssthresh = TCP_MSS;
        s->cwnd = TCP_MSS;

        s->seq = s->retx_seq;
        send_seg(s, TCP_FLAG_PSH | TCP_FLAG_ACK, s->retx_data, s->retx_len);
    }
}

/* -- Incoming packet dispatcher -------------------------------------------- */
void tcp_handle(ip_hdr_t *ip_hdr, void *pkt, int len)
{
    if (len < 20) return;

    uint8_t  *tcp      = (uint8_t *)pkt;
    uint16_t  src_port = ntohs(*(uint16_t *)(tcp + 0));
    uint16_t  dst_port = ntohs(*(uint16_t *)(tcp + 2));
    uint32_t  pkt_seq  = ntohl(*(uint32_t *)(tcp + 4));
    uint32_t  pkt_ack  = ntohl(*(uint32_t *)(tcp + 8));
    int       data_off = (*(uint8_t *)(tcp + 12)) >> 4;
    uint8_t   flags    = *(uint8_t  *)(tcp + 13);
    int       hdr_len  = data_off * 4;
    int       data_len = len - hdr_len;
    if (data_len < 0) data_len = 0;
    const uint8_t *data = tcp + hdr_len;

    tcp_sock_t *s = 0;
    int i;
    for (i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_sock_t *c = &socks[i];
        if (!c->used || c->state == TCP_LISTEN) continue;
        if (c->src_port == dst_port &&
            c->dst_port == src_port &&
            c->dst_ip   == ip_hdr->src_ip) {
            s = c; break;
        }
    }

    /* LISTEN: incoming SYN -- spawn new socket */
    if (!s && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        for (i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (!socks[i].used) continue;
            if (socks[i].state != TCP_LISTEN) continue;
            if (socks[i].src_port != dst_port) continue;

            int new_fd = tcp_socket();
            if (new_fd < 0) return;
            tcp_sock_t *ns = &socks[new_fd];
            ns->dst_ip   = ip_hdr->src_ip;
            ns->dst_port = src_port;
            ns->src_port = dst_port;
            ns->seq      = 0x2000;
            ns->ack      = pkt_seq + 1;
            ns->state    = TCP_SYN_RECEIVED;
            send_seg(ns, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
            socks[i].accept_queue[socks[i].accept_tail % 4] = new_fd;
            socks[i].accept_tail++;
            return;
        }
        return;
    }

    if (!s) return;

    if (flags & TCP_FLAG_RST) {
        retx_clear(s);
        if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }
        s->state = TCP_CLOSED;
        return;
    }

    /* SYN-ACK (client side) */
    if (s->state == TCP_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            s->ack     = pkt_seq + 1;
            s->seq     = pkt_ack;
            s->snd_una = s->seq;
            s->state   = TCP_ESTABLISHED;
            send_seg(s, TCP_FLAG_ACK, 0, 0);
        }
        return;
    }

    /* ACK of our SYN-ACK (server side) */
    if (s->state == TCP_SYN_RECEIVED) {
        if (flags & TCP_FLAG_ACK) {
            s->seq     = pkt_ack;
            s->snd_una = s->seq;
            s->state   = TCP_ESTABLISHED;
        }
        return;
    }

    if (s->state != TCP_ESTABLISHED &&
        s->state != TCP_FIN_WAIT_1  &&
        s->state != TCP_FIN_WAIT_2  &&
        s->state != TCP_CLOSE_WAIT  &&
        s->state != TCP_CLOSING     &&
        s->state != TCP_LAST_ACK)
        return;

    /* Data */
    if (data_len > 0 && pkt_seq == s->ack) {
        rx_append(s, data, data_len);
        s->ack += (uint32_t)data_len;
        send_seg(s, TCP_FLAG_ACK, 0, 0);
    }

    /* ACK */
    if (flags & TCP_FLAG_ACK) {
        if (pkt_ack > s->snd_una) {
            s->snd_una = pkt_ack;
            if (s->cwnd < s->ssthresh)
                s->cwnd += TCP_MSS;
            else
                s->cwnd += (uint32_t)(TCP_MSS * TCP_MSS) / s->cwnd;
            if (s->retx_len > 0 &&
                pkt_ack >= s->retx_seq + (uint32_t)s->retx_len)
                retx_clear(s);
        }
        if (s->state == TCP_FIN_WAIT_1) s->state = TCP_FIN_WAIT_2;
        if (s->state == TCP_CLOSING)    s->state = TCP_TIME_WAIT;
        if (s->state == TCP_LAST_ACK)   s->state = TCP_CLOSED;
    }

    /* FIN */
    if (flags & TCP_FLAG_FIN) {
        s->ack++;
        s->rx_closed = 1;
        send_seg(s, TCP_FLAG_ACK, 0, 0);
        if (s->state == TCP_ESTABLISHED)     s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT_1) s->state = TCP_CLOSING;
        else if (s->state == TCP_FIN_WAIT_2) s->state = TCP_TIME_WAIT;
    }
}

/* -- Backward-compatible blocking API ------------------------------------- */
static int compat_fd = -1;

int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    if (compat_fd >= 0) { tcp_close2(compat_fd); compat_fd = -1; }
    int fd = tcp_socket();
    if (fd < 0) return -1;
    if (tcp_connect2(fd, dst_ip, dst_port) != 0) {
        tcp_close2(fd);
        return -1;
    }
    compat_fd = fd;
    return 0;
}

int tcp_send(void *data, int len)
{
    return tcp_send2(compat_fd, data, len);
}

int tcp_recv(uint8_t *buf, int max_len)
{
    return tcp_recv2(compat_fd, buf, max_len);
}

void tcp_close(void)
{
    if (compat_fd < 0) return;
    tcp_close2(compat_fd);
    compat_fd = -1;
}

/* Read-only query API for Network Monitor */
int tcp_get_connections(tcp_conn_info_t *out, int max)
{
    int n = 0;
    int i;
    for (i = 0; i < TCP_MAX_SOCKETS && n < max; i++) {
        if (!socks[i].used) continue;
        out[n].fd       = i;
        out[n].state    = socks[i].state;
        out[n].dst_ip   = socks[i].dst_ip;
        out[n].dst_port = socks[i].dst_port;
        out[n].src_port = socks[i].src_port;
        n++;
    }
    return n;
}
