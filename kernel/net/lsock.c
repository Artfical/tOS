#include "lsock.h"
#include "ip.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "string.h"
#include "csprng.h"
#include "memory.h"

/*
 * Linux sockets over tOS's network stack, for ELF programs. AF_INET only (AF_UNIX, AF_INET6
 * and netlink answer "address family not supported", which glibc, Node and npm all handle):
 *   SOCK_STREAM - a TCP connection (kernel/net/tcp.c, its non-blocking calls)
 *   SOCK_DGRAM  - UDP on a port of its own (one waiting datagram, as udp.c keeps them)
 * Addresses are struct sockaddr_in: family, port (big-endian), then the IPv4 address, whose
 * bytes are in the same order tOS keeps net_ip in.
 */

#define E_AGAIN        11
#define E_ADDRINUSE    98
#define E_NOENT        2
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
    int      listening;         /* TCP: accepting connections on lport */
    int      err;               /* SO_ERROR, positive, until read */
    int      shut_wr;
    uint16_t lport;             /* UDP: the local port (0 until it sends or binds) */
    uint32_t peer_ip;
    uint16_t peer_port;
    int      peer_set;
    int      refs;              /* descriptors on it (dup, fork) */
    /* AF_UNIX: a byte ring of its own for what the other end sent, and the other end */
    int      un;                /* 1: AF_UNIX stream */
    int      un_peer;           /* the other end's index, -1 when there is none (never connected, or it closed) */
    int      un_was_conn;       /* it was connected once (so a lost peer reads as end of file) */
    uint8_t *un_rx;
    uint32_t un_head, un_len;
    int      un_bl[8], un_bl_n; /* listening: connections waiting to be accepted (sockets already made) */
    char     un_path[108];
    uint32_t un_seq;            /* when it was bound (the newest listener of a path wins) */
} lsock_t;

#define UN_RING 65536u
static uint32_t un_seq_counter;

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
    int kind = type & 0xF;
    if (domain == 1) {                                          /* AF_UNIX: streams (and seqpackets, as streams) */
        if (kind != 1 && kind != 5) return -E_AFNOSUPPORT;
        for (int i = 0; i < LSOCK_MAX; i++) {
            if (socks[i].used) continue;
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = 1; socks[i].refs = 1; socks[i].stream = 1; socks[i].tcp = -1;
            socks[i].un = 1; socks[i].un_peer = -1;
            open_count++;
            return i;
        }
        return -E_MFILE;
    }
    if (domain != 2) return -E_AFNOSUPPORT;
    if (kind != 1 && kind != 2) return -E_INVAL;
    for (int i = 0; i < LSOCK_MAX; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].refs = 1;
        socks[i].stream = kind == 1;
        socks[i].tcp = -1;
        open_count++;
        return i;
    }
    return -E_MFILE;
}

void lsock_ref(int s)
{
    lsock_t *k = get(s);
    if (k) k->refs++;
}

void lsock_close(int s)
{
    lsock_t *k = get(s);
    if (!k) return;
    if (--k->refs > 0) return;                  /* another descriptor still has it */
    if (k->un) {
        for (int i = 0; i < k->un_bl_n; i++) lsock_close(k->un_bl[i]);     /* connections nobody accepted */
        if (k->un_peer >= 0 && socks[k->un_peer].used) socks[k->un_peer].un_peer = -1;
        free(k->un_rx);
        memset(k, 0, sizeof(*k));
        if (open_count > 0) open_count--;
        return;
    }
    if (k->tcp >= 0) tcp_release(k->tcp);
    if (!k->stream && k->lport) udp_close(k->lport);
    memset(k, 0, sizeof(*k));
    if (open_count > 0) open_count--;
}

int lsock_bind(int s, const uint8_t *addr, uint32_t len)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (k->un) return -E_INVAL;                     /* lsock_unix_bind() does these */
    uint32_t ip; uint16_t port;
    int r = parse_addr(addr, len, &ip, &port);
    if (r < 0) return r;
    if (k->stream) {                      /* TCP: the port a listen() will use (0: a free one) */
        if (k->lport) return -E_INVAL;
        static uint16_t next_port = 40000;
        k->lport = port ? port : next_port++;
        return 0;
    }
    if (k->lport) return -E_INVAL;
    if (!port) return udp_port(k);
    if (udp_open(port) < 0) return -E_NOBUFS;
    k->lport = port;
    return 0;
}

int lsock_listen(int s, int backlog)
{
    (void)backlog;
    lsock_t *k = get(s);
    if (!k || !k->stream) return -E_OPNOTSUPP;
    if (k->un) {
        if (!k->un_path[0]) return -E_INVAL;
        if (k->un_peer >= 0) return -E_ISCONN;
        k->listening = 1;
        return 0;
    }
    if (k->listening) return 0;
    if (k->tcp >= 0) return -E_ISCONN;
    if (!k->lport) { static uint16_t next_port = 41000; k->lport = next_port++; }
    int t = tcp_socket();
    if (t < 0) return -E_NOBUFS;
    if (tcp_listen(t, k->lport) != 0) { tcp_release(t); return -E_INVAL; }
    k->tcp = t;
    k->listening = 1;
    return 0;
}

/* a waiting connection becomes a socket of its own */
int lsock_accept(int s, uint8_t *addr, uint32_t *len)
{
    lsock_t *k = get(s);
    if (!k || !k->listening) return -E_INVAL;
    if (k->un) {
        if (!k->un_bl_n) return -E_AGAIN;
        int c = k->un_bl[0];
        for (int i = 1; i < k->un_bl_n; i++) k->un_bl[i - 1] = k->un_bl[i];
        k->un_bl_n--;
        if (addr && len) { uint8_t a[2] = { 1, 0 }; uint32_t n = *len < 2 ? *len : 2; memcpy(addr, a, n); *len = 2; }
        return c;
    }
    int t = tcp_accept_nb(k->tcp);
    if (t < 0) return -E_AGAIN;
    for (int i = 0; i < LSOCK_MAX; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = 1;
        socks[i].refs = 1;
        socks[i].stream = 1;
        socks[i].tcp = t;
        socks[i].lport = k->lport;
        tcp_peer(t, &socks[i].peer_ip, &socks[i].peer_port);
        socks[i].peer_set = 1;
        open_count++;
        if (addr && len) put_addr(addr, len, socks[i].peer_ip, socks[i].peer_port);
        return i;
    }
    tcp_release(t);
    return -E_MFILE;
}

int lsock_connect_start(int s, const uint8_t *addr, uint32_t len)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (k->un) return -E_AFNOSUPPORT;
    uint32_t ip; uint16_t port;
    int r = parse_addr(addr, len, &ip, &port);
    if (r < 0) return r;
    if (!k->stream) {                     /* UDP: just where send() goes */
        k->peer_ip = ip; k->peer_port = port; k->peer_set = 1;
        return udp_port(k);
    }
    if (net_ip && ip == net_ip) ip = ip_loopback_addr();       /* a connection to this machine's own address stays inside it */
    if (k->connecting) return -E_ALREADY;
    if (k->tcp >= 0) return -E_ISCONN;
    if (!net_ip && !ip_is_loopback(ip)) return -E_NETUNREACH;
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
    if (k->un) return k->un_peer >= 0 ? 0 : -E_NOTCONN;
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
    if (k->un) {
        if (k->un_peer < 0 || k->shut_wr) return k->un_was_conn || k->shut_wr ? -E_PIPE : -E_DESTADDRREQ;
        lsock_t *q = &socks[k->un_peer];
        if (!len) return 0;
        uint32_t room = UN_RING - q->un_len;
        if (!room) return -E_AGAIN;
        uint32_t n = len < room ? len : room;
        if (!q->un_rx) { q->un_rx = malloc(UN_RING); if (!q->un_rx) return -E_NOBUFS; }
        for (uint32_t i = 0; i < n; i++) q->un_rx[(q->un_head + q->un_len + i) % UN_RING] = data[i];
        q->un_len += n;
        return (int)n;
    }
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
    if (net_ip && ip == net_ip) ip = ip_loopback_addr();
    if (!net_ip && !ip_is_loopback(ip)) return -E_NETUNREACH;
    if (udp_send(ip, port, k->lport, (void *)data, (int)len) < 0) return -E_HOSTUNREACH;
    return (int)len;
}

int lsock_recv(int s, uint8_t *buf, uint32_t len, uint8_t *from, uint32_t *fromlen, int peek)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (k->un) {
        if (k->un_len) {
            uint32_t n = len < k->un_len ? len : k->un_len;
            for (uint32_t i = 0; i < n; i++) buf[i] = k->un_rx[(k->un_head + i) % UN_RING];
            if (!peek) { k->un_head = (k->un_head + n) % UN_RING; k->un_len -= n; }
            return (int)n;
        }
        if (k->un_peer < 0) return k->un_was_conn ? 0 : -E_NOTCONN;       /* the other end is gone: end of file */
        if (socks[k->un_peer].shut_wr) return 0;
        return -E_AGAIN;
    }
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
    if (k->un) {
        if (k->listening) return k->un_bl_n ? 0x001 : 0;
        if (k->un_len) r |= 0x001;
        if (k->un_peer >= 0) {
            lsock_t *q = &socks[k->un_peer];
            if (q->un_len < UN_RING) r |= 0x004;
            if (q->shut_wr && !k->un_len) r |= 0x001 | 0x2000;
        } else if (k->un_was_conn) {
            r |= 0x004 | 0x2000 | (k->un_len ? 0 : 0x001 | 0x010);        /* the peer is gone */
        } else r |= 0x004;
        if (k->err) r |= 0x008;
        return r;
    }
    if (k->stream) {
        if (k->connecting) {
            int c = lsock_connect_check(s);
            if (c == -E_INPROGRESS) return 0;
            if (c < 0) { k->err = -c; return 0x004 | 0x008; }    /* OUT | ERR: connect failed */
        }
        if (k->listening) return tcp_accept_ready(k->tcp) ? 0x001 : 0;
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
        case 39: *value = k->un ? 1 : 2; return 0;                 /* SO_DOMAIN */
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
    if (k->un) {
        uint8_t a[112]; memset(a, 0, sizeof(a)); a[0] = 1;
        uint32_t total = 2;
        if (!peer && k->un_path[0]) {
            uint32_t pl = (uint32_t)strlen(k->un_path);
            if (k->un_path[0] == '@') { memcpy(a + 3, k->un_path + 1, pl - 1); total = 3 + pl - 1; }   /* abstract: a NUL, then the name */
            else { memcpy(a + 2, k->un_path, pl + 1); total = 2 + pl + 1; }
        } else if (peer && k->un_peer < 0) return -E_NOTCONN;
        uint32_t n = *len < total ? *len : total;
        memcpy(addr, a, n);
        *len = total;
        return 0;
    }
    if (peer) {
        if (!k->peer_set || (k->stream && (k->tcp < 0 || k->connecting))) return -E_NOTCONN;
        put_addr(addr, len, k->peer_ip, k->peer_port);
        return 0;
    }
    uint16_t port = k->lport;
    if (k->stream && k->tcp >= 0) tcp_local(k->tcp, &port);
    put_addr(addr, len, k->peer_set && ip_is_loopback(k->peer_ip) ? ip_loopback_addr() : net_ip, port);
    return 0;
}

int lsock_shutdown(int s, int how)
{
    lsock_t *k = get(s);
    if (!k) return -E_INVAL;
    if (how < 0 || how > 2) return -E_INVAL;
    if (k->un) { if (k->un_peer < 0 && !k->un_was_conn) return -E_NOTCONN; if (how >= 1) k->shut_wr = 1; return 0; }
    if (k->stream && k->tcp < 0) return -E_NOTCONN;
    if (how >= 1 && !k->shut_wr) { k->shut_wr = 1; if (k->stream && k->tcp >= 0 && !k->connecting) tcp_shutdown_wr(k->tcp); }
    return 0;
}

int lsock_pending(int s)
{
    lsock_t *k = get(s);
    if (!k) return 0;
    if (k->un) return (int)k->un_len;
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

int lsock_is_unix(int s) { lsock_t *k = get(s); return k && k->un; }

int lsock_unix_pair(int *a, int *b)
{
    int x = lsock_create(1, 1, 0);
    if (x < 0) return x;
    int y = lsock_create(1, 1, 0);
    if (y < 0) { lsock_close(x); return y; }
    socks[x].un_peer = y; socks[y].un_peer = x;
    socks[x].un_was_conn = socks[y].un_was_conn = 1;
    *a = x; *b = y;
    return 0;
}

int lsock_unix_peer(int s) { lsock_t *k = get(s); return k && k->un ? k->un_peer : -1; }

int lsock_unix_bind(int s, const char *abs_path)
{
    lsock_t *k = get(s);
    if (!k || !k->un) return -E_INVAL;
    if (k->un_path[0]) return -E_INVAL;
    if (strlen(abs_path) >= sizeof(k->un_path)) return -E_INVAL;
    strcpy(k->un_path, abs_path);
    k->un_seq = ++un_seq_counter;
    return 0;
}

int lsock_unix_connect(int s, const char *abs_path)
{
    lsock_t *k = get(s);
    if (!k || !k->un) return -E_INVAL;
    if (k->un_peer >= 0) return -E_ISCONN;
    lsock_t *l = 0;
    for (int i = 0; i < LSOCK_MAX; i++)
        if (socks[i].used && socks[i].un && socks[i].listening && !strcmp(socks[i].un_path, abs_path) && (!l || socks[i].un_seq > l->un_seq)) l = &socks[i];
    if (!l) return -E_CONNREFUSED;
    if (l->un_bl_n >= 8) return -E_AGAIN;
    int ci = (int)(k - socks), c = -1;
    for (int i = 0; i < LSOCK_MAX; i++) if (!socks[i].used) { c = i; break; }
    if (c < 0) return -E_MFILE;
    lsock_t *sv = &socks[c];
    memset(sv, 0, sizeof(*sv));
    sv->used = 1; sv->refs = 1; sv->stream = 1; sv->tcp = -1; sv->un = 1;
    sv->un_peer = ci; sv->un_was_conn = 1;
    strcpy(sv->un_path, l->un_path);
    open_count++;
    k->un_peer = c; k->un_was_conn = 1;
    l->un_bl[l->un_bl_n++] = c;
    return 0;
}
