#include "lsock.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "string.h"
#include "csprng.h"

/*
 * Linux sockets over tOS's network stack, for ELF programs. AF_INET only (AF_UNIX, AF_INET6
 * and netlink answer "address family not supported", which glibc, Node and npm all handle):
 *   SOCK_STREAM - a TCP connection (kernel/net/tcp.c, its non-blocking calls)
 *   SOCK_DGRAM  - UDP on a port of its own (one waiting datagram, as udp.c keeps them)
 * Addresses are struct sockaddr_in: family, port (big-endian), then the IPv4 address, whose
 * bytes are in the same order tOS keeps net_ip in.
 */

#define E_AGAIN        11
#define E_PIPE         32
#define E_INVAL        22
#define E_MFILE        24
#define E_DESTADDRREQ  89
#define E_MSGSIZE      90
#define E_NOPROTOOPT   92
#define E_OPNOTSUPP    95
#define E_AFNOSUPPORT  97
#define E_NETUNREACH   101
#define E_NOBUFS       105
#define E_ISCONN       106
#define E_NOTCONN      107
#define E_TIMEDOUT     110
#define E_CONNREFUSED  111
#define E_HOSTUNREACH  113
#define E_ALREADY      114
#define E_INPROGRESS   115

typedef struct {
    int      used;
    int      stream;            /* 1 TCP, 0 UDP */
    int      tcp;               /* the TCP socket, -1 until connect */
    int      connecting;
    int      err;               /* SO_ERROR, positive, until read */
    int      shut_wr;
    uint16_t lport;             /* UDP: the local port (0 until it sends or binds) */
    uint32_t peer_ip;
    uint16_t peer_port;
    int      peer_set;
} lsock_t;

static lsock_t socks[LSOCK_MAX];
static int open_count;

static lsock_t *get(int s)
{
    if (s < 0 || s >= LSOCK_MAX || !socks[s].used) return 0;
    return &socks[s];
}

static int parse_addr(const uint8_t *a, uint32_t len, uint32_t *ip, uint16_t *port)
{
    if (!a || len < 8) return -E_INVAL;
    uint16_t family = (uint16_t)(a[0] | (a[1] << 8));
    if (family != 2) return -E_AFNOSUPPORT;
    *port = (uint16_t)((a[2] << 8) | a[3]);
    memcpy(ip, a + 4, 4);
    return 0;
}

static void put_addr(uint8_t *out, uint32_t *len, uint32_t ip, uint16_t port)
{
    uint8_t a[16];
    memset(a, 0, sizeof(a));
    a[0] = 2;
    a[2] = (uint8_t)(port >> 8);
    a[3] = (uint8_t)port;
    memcpy(a + 4, &ip, 4);
    uint32_t n = *len < 16 ? *len : 16;
    memcpy(out, a, n);
    *len = 16;
}

static int tcp_error(int r)
{
    if (r == TCP_ERR_REFUSED) return -E_CONNREFUSED;
    if (r == TCP_ERR_TIMEOUT) return -E_TIMEDOUT;
    if (r == TCP_ERR_NOSOCK) return -E_NOBUFS;
    return -E_HOSTUNREACH;            /* ARP / route failures */
}

static int udp_port(lsock_t *k)
{
    if (k->lport) return 0;
    for (int tries = 0; tries < 32; tries++) {
        uint16_t p = (uint16_t)(32768 + csprng_u32() % 16384);
        if (udp_port_open(p)) continue;
        if (udp_open(p) < 0) return -E_NOBUFS;
        k->lport = p;
        return 0;
    }
    return -E_NOBUFS;
}

int lsock_create(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != 2) return -E_AFNOSUPPORT;
    int kind = type & 0xF;
    if (kind != 1 && kind != 2) return -E_INVAL;
    for (int i = 0; i < LSOCK_MAX; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].stream = kind == 1;
        socks[i].tcp = -1;
        open_count++;
        return i;
    }
    return -E_MFILE;
}

void lsock_close(int s)
{
    lsock_t *k = get(s);
    if (!k) return;
    if (k->tcp >= 0) tcp_release(k->tcp);
    if (!k->stream && k->lport) udp_close(k->lport);
    memset(k, 0, sizeof(*k));
    if (open_count > 0) open_count--;
}

int lsock_bind(int s, const uint8_t *addr, uint32_t len)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    uint32_t ip; uint16_t port;
    int r = parse_addr(addr, len, &ip, &port);
    if (r < 0) return r;
    if (k->stream) return 0;              /* the port is chosen at connect (servers are not supported) */
    if (k->lport) return -E_INVAL;
    if (!port) return udp_port(k);
    if (udp_open(port) < 0) return -E_NOBUFS;
    k->lport = port;
    return 0;
}

int lsock_connect_start(int s, const uint8_t *addr, uint32_t len)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    uint32_t ip; uint16_t port;
    int r = parse_addr(addr, len, &ip, &port);
    if (r < 0) return r;
    if (!k->stream) {                     /* UDP: just where send() goes */
        k->peer_ip = ip; k->peer_port = port; k->peer_set = 1;
        return udp_port(k);
    }
    if (k->connecting) return -E_ALREADY;
    if (k->tcp >= 0) return -E_ISCONN;
    if (!net_ip) return -E_NETUNREACH;
    int t = tcp_socket();
    if (t < 0) return -E_NOBUFS;
    r = tcp_connect_begin(t, ip, port);
    if (r != 0) { tcp_release(t); return tcp_error(r); }
    k->tcp = t;
    k->peer_ip = ip; k->peer_port = port; k->peer_set = 1;
    k->connecting = 1;
    return -E_INPROGRESS;
}

int lsock_connect_check(int s)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (!k->stream) return 0;
    if (!k->connecting) {
        if (k->err) { int e = k->err; k->err = 0; return -e; }
        return k->tcp >= 0 ? 0 : -E_NOTCONN;
    }
    int r = tcp_connect_poll(k->tcp);
    if (r == 0) return -E_INPROGRESS;
    k->connecting = 0;
    if (r == 1) return 0;
    k->err = -tcp_error(r);
    tcp_release(k->tcp);
    k->tcp = -1;
    return tcp_error(r);
}

int lsock_send(int s, const uint8_t *data, uint32_t len, const uint8_t *to, uint32_t tolen)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (k->stream) {
        if (k->connecting) { int r = lsock_connect_check(s); if (r < 0) return r == -E_INPROGRESS ? -E_AGAIN : r; }
        if (k->tcp < 0 || !tcp_is_established(k->tcp)) return -E_PIPE;
        if (k->shut_wr) return -E_PIPE;
        if (!len) return 0;
        if (tcp_send2(k->tcp, (void *)data, (int)len) != 0) return -E_PIPE;
        return (int)len;
    }
    uint32_t ip = k->peer_ip; uint16_t port = k->peer_port;
    if (to && tolen) {
        int r = parse_addr(to, tolen, &ip, &port);
        if (r < 0) return r;
    } else if (!k->peer_set) return -E_DESTADDRREQ;
    if (len > 65507) return -E_MSGSIZE;
    int r = udp_port(k);
    if (r < 0) return r;
    if (!net_ip) return -E_NETUNREACH;
    if (udp_send(ip, port, k->lport, (void *)data, (int)len) < 0) return -E_HOSTUNREACH;
    return (int)len;
}

int lsock_recv(int s, uint8_t *buf, uint32_t len, uint8_t *from, uint32_t *fromlen, int peek)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (k->stream) {
        if (k->connecting) { int r = lsock_connect_check(s); if (r < 0) return r == -E_INPROGRESS ? -E_AGAIN : r; }
        if (k->tcp < 0) return k->err ? -k->err : -E_NOTCONN;
        if (peek) {
            int n = tcp_rx_ready(k->tcp);
            return n > 0 ? 0 : n < 0 ? 0 : -E_AGAIN;    /* MSG_PEEK: whether, not what */
        }
        int n = tcp_recv_nb(k->tcp, buf, (int)len);
        if (n == TCP_ERR_RECV_TIMEOUT) return -E_AGAIN;
        if (n > 0 && from && fromlen) put_addr(from, fromlen, k->peer_ip, k->peer_port);
        return n;
    }
    if (!k->lport) return -E_AGAIN;
    uint32_t ip = 0; uint16_t port = 0;
    int n = udp_listen(k->lport, buf, (int)len, &ip, &port);
    if (n < 0) return -E_AGAIN;
    if (from && fromlen) put_addr(from, fromlen, ip, port);
    return n;
}

uint32_t lsock_ready(int s)
{
    lsock_t *k = get(s);
    if (!k) return 0x10;                                          /* EPOLLHUP */
    uint32_t r = 0;
    if (k->stream) {
        if (k->connecting) {
            int c = lsock_connect_check(s);
            if (c == -E_INPROGRESS) return 0;
            if (c < 0) { k->err = -c; return 0x004 | 0x008; }    /* OUT | ERR: connect failed */
        }
        if (k->err) r |= 0x008;
        if (k->tcp < 0) return r | 0x004 | 0x010;                 /* not connected: OUT | HUP */
        int n = tcp_rx_ready(k->tcp);
        if (n > 0) r |= 0x001;
        if (n < 0) r |= 0x001 | 0x2000;                           /* IN | RDHUP: the peer finished */
        if (tcp_is_established(k->tcp)) r |= 0x004;
        else if (n < 0) r |= 0x010;
        return r;
    }
    if (k->lport && udp_has_data(k->lport) >= 0) r |= 0x001;
    return r | 0x004;
}

int lsock_getsockopt(int s, int level, int name, int *value)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (level == 1) {                                            /* SOL_SOCKET */
        switch (name) {
        case 4:  *value = k->err; k->err = 0; return 0;           /* SO_ERROR */
        case 3:  *value = k->stream ? 1 : 2; return 0;            /* SO_TYPE */
        case 39: *value = 2; return 0;                            /* SO_DOMAIN */
        case 7:  case 8: *value = 64 * 1024; return 0;            /* SO_SNDBUF, SO_RCVBUF */
        default: *value = 0; return 0;
        }
    }
    if (level == 6) { *value = 0; return 0; }                    /* IPPROTO_TCP: TCP_NODELAY &c. */
    return -E_NOPROTOOPT;
}

int lsock_name(int s, int peer, uint8_t *addr, uint32_t *len)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (peer) {
        if (!k->peer_set || (k->stream && (k->tcp < 0 || k->connecting))) return -E_NOTCONN;
        put_addr(addr, len, k->peer_ip, k->peer_port);
        return 0;
    }
    uint16_t port = k->lport;
    if (k->stream && k->tcp >= 0) tcp_local(k->tcp, &port);
    put_addr(addr, len, net_ip, port);
    return 0;
}

int lsock_shutdown(int s, int how)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (how < 0 || how > 2) return -E_INVAL;
    if (k->stream && k->tcp < 0) return -E_NOTCONN;
    if (how >= 1) k->shut_wr = 1;
    return 0;
}

int lsock_pending(int s)
{
    lsock_t *k = get(s);
    if (!k) return 0;
    if (k->stream) {
        if (k->tcp < 0) return 0;
        int n = tcp_rx_ready(k->tcp);
        return n > 0 ? n : 0;
    }
    int n = k->lport ? udp_has_data(k->lport) : -1;
    return n > 0 ? n : 0;
}

int lsock_is_stream(int s)
{
    lsock_t *k = get(s);
    return k && k->stream;
}

void lsock_pump(void)
{
    if (open_count > 0) tcp_pump();
}
