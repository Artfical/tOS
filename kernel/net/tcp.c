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
#include "route.h"
#include "string.h"
#include "memory.h"
#include "scheduler.h"
#include "debugmon.h"
#include "terminal.h"
#include "klog.h"
#include "csprng.h"

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
    uint32_t snd_wnd;   /* window the peer advertised in its latest valid ACK */

    /* the receive buffer is a ring of rx_cap bytes: rx_len in-order bytes from rx_head wait for the
     * program, and segments that arrived ahead of a gap sit beyond them (ooo) until it is filled */
    uint8_t *rx_buf;
    int      rx_head;
    int      rx_len;
    int      rx_cap;
    int      rx_closed;
    int      adv_wnd;           /* the window our last segment told the peer */
    int      ack_pending;       /* data segments taken in whose acknowledgement is still to be sent */
    uint8_t  ws_ok;             /* window scaling was agreed in the handshake */
    uint8_t  snd_shift;         /* the peer's scale: its window field counts in units of 1 << snd_shift */
    uint16_t peer_mss;          /* the largest segment the peer takes */
    int      ooo_n;
    struct { uint32_t seq; int len; } ooo[8];

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

    /* Set by tcp_handle()'s RST branch so tcp_connect2() can tell
     * "the peer actively refused" apart from "no reply arrived" --
     * both used to just leave state == TCP_CLOSED, indistinguishable
     * from the outside. */
    int      got_rst;

    int      syn_age;   /* tcp_tick() ticks spent in SYN_RECEIVED */

    /* a non-blocking connect (tcp_connect_begin): the SYN to resend, and when */
    uint32_t syn_seq;
    uint32_t syn_next_ms;
    uint32_t syn_deadline_ms;
} tcp_sock_t;

static tcp_sock_t socks[TCP_MAX_SOCKETS];
static uint16_t   tcp_ip_id = 0;

/* Sequence-number arithmetic modulo 2^32 (RFC 793 / 1982): plain < and >
 * misorder values once the counter wraps. */
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)

/* -- Helpers --------------------------------------------------------------- */
static tcp_sock_t *get_sock(int fd)
{
    if (fd < 0 || fd >= TCP_MAX_SOCKETS) return 0;
    if (!socks[fd].used) return 0;
    return &socks[fd];
}

/* Random ephemeral port in 49152..65535 that no socket is using. A counter
 * (the old code) lets an off-path attacker predict the 4-tuple. */
static uint16_t alloc_port(void)
{
    for (int tries = 0; tries < 64; tries++) {
        uint16_t p = (uint16_t)(49152 + csprng_u32() % 16384);
        int busy = 0;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++)
            if (socks[i].used && socks[i].src_port == p) { busy = 1; break; }
        if (!busy) return p;
    }
    return (uint16_t)(49152 + csprng_u32() % 16384);
}

/* Receive buffer: 4 MB where there is memory for it (a 64 KB window caps a download at 64 KB per round
 * trip; 1 Gbit/s over 30 ms needs about 4 MB), the window field scaled by 1 << TCP_RCV_SHIFT to say so. Data beyond what is free is
 * not acknowledged, so the peer retransmits once the program has read some. */
#define TCP_RX_MAX    (4 * 1024 * 1024)
#define TCP_RX_MIN    (64 * 1024)
#define TCP_RCV_SHIFT 7              /* 65535 << 7 reaches 8 MB */

static int rx_free(const tcp_sock_t *s)
{
    int cap = s->rx_cap ? s->rx_cap : TCP_RX_MAX;
    return s->rx_len >= cap ? 0 : cap - s->rx_len;
}

static int rx_ensure(tcp_sock_t *s)
{
    if (s->rx_buf) return 1;
    /* as much as there is memory for: 4 MB, then a quarter of it each time that fails */
    int cap = TCP_RX_MAX;
    uint8_t *b = (uint8_t *)malloc((size_t)cap);
    while (!b && cap > TCP_RX_MIN) { cap /= 4; if (cap < TCP_RX_MIN) cap = TCP_RX_MIN; b = (uint8_t *)malloc((size_t)cap); }
    if (!b) return 0;
    s->rx_buf = b; s->rx_cap = cap; s->rx_head = 0; s->rx_len = 0; s->ooo_n = 0;
    return 1;
}

static void rx_release(tcp_sock_t *s)
{
    if (s->rx_buf) free(s->rx_buf);
    s->rx_buf = 0; s->rx_cap = 0; s->rx_head = 0; s->rx_len = 0; s->ooo_n = 0;
}

/* bytes of the ring at `off` bytes past the end of the in-order data */
static void rx_put(tcp_sock_t *s, int off, const uint8_t *data, int len)
{
    int at = (s->rx_head + s->rx_len + off) % s->rx_cap;
    int first = s->rx_cap - at < len ? s->rx_cap - at : len;
    memcpy(s->rx_buf + at, data, (size_t)first);
    if (first < len) memcpy(s->rx_buf, data + first, (size_t)(len - first));
}

/* what the program takes out of the ring */
static int rx_take(tcp_sock_t *s, uint8_t *dst, int max)
{
    int n = s->rx_len < max ? s->rx_len : max;
    int first = s->rx_cap - s->rx_head < n ? s->rx_cap - s->rx_head : n;
    memcpy(dst, s->rx_buf + s->rx_head, (size_t)first);
    if (first < n) memcpy(dst + first, s->rx_buf, (size_t)(n - first));
    s->rx_head = (s->rx_head + n) % s->rx_cap;
    s->rx_len -= n;
    if (s->rx_len == 0 && s->ooo_n == 0) s->rx_head = 0;
    return n;
}

/* The segment that starts at s->ack: 1 when all of it fits (and the ooo ranges it reaches are joined
 * on), 0 when there is no room. */
static int rx_append(tcp_sock_t *s, const uint8_t *data, int len)
{
    if (len <= 0) return 1;
    if (!rx_ensure(s) || len > rx_free(s)) return 0;
    rx_put(s, 0, data, len);
    s->rx_len += len;
    s->ack += (uint32_t)len;
    for (int again = 1; again && s->ooo_n;) {                 /* ranges that now touch the data */
        again = 0;
        for (int i = 0; i < s->ooo_n; i++) {
            if (!SEQ_LEQ(s->ooo[i].seq, s->ack)) continue;
            uint32_t end = s->ooo[i].seq + (uint32_t)s->ooo[i].len;
            if (SEQ_GT(end, s->ack)) {
                int adv = (int)(end - s->ack);                /* the bytes were put in place on arrival */
                s->rx_len += adv;
                s->ack = end;
            }
            s->ooo[i] = s->ooo[--s->ooo_n];
            again = 1;
            break;
        }
    }
    return 1;
}

/* A segment ahead of a gap (seq after s->ack): kept in its place in the ring, so the peer does not have
 * to send it again once the hole is filled. */
static void rx_hold(tcp_sock_t *s, uint32_t seq, const uint8_t *data, int len)
{
    uint32_t off = seq - s->ack;
    if (!rx_ensure(s) || off >= (uint32_t)rx_free(s) || len > rx_free(s) - (int)off) return;
    for (int i = 0; i < s->ooo_n; i++)                        /* already held (a retransmission) */
        if (s->ooo[i].seq == seq && s->ooo[i].len >= len) return;
    if (s->ooo_n >= (int)(sizeof(s->ooo) / sizeof(s->ooo[0]))) return;
    rx_put(s, (int)off, data, len);
    s->ooo[s->ooo_n].seq = seq;
    s->ooo[s->ooo_n].len = len;
    s->ooo_n++;
}

/* The one's complement sum of `len` bytes as 16-bit words in the byte order of memory, eight bytes at a
 * time (every frame is summed: the plain loop was the stack's biggest cost per packet). The result is
 * folded to 16 bits and byte-swapped into network order. */
static uint32_t csum_net(const uint8_t *p, int len)
{
    uint64_t sum = 0;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        sum += (v & 0xFFFFFFFFu) + (v >> 32);
        p += 8; len -= 8;
    }
    while (len >= 2) { uint16_t w; memcpy(&w, p, 2); sum += w; p += 2; len -= 2; }
    if (len) sum += *p;                                   /* a lone last byte is the low half of its word */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint32_t)(((sum & 0xFF) << 8) | (sum >> 8));  /* memory order -> network order */
}

/* -- Packet builder -------------------------------------------------------- */
static int send_seg(tcp_sock_t *s, uint8_t flags, const void *payload, int plen)
{
    /* Resolve via the routing table, not the raw destination IP directly
     * — arp_resolve(s->dst_ip, ...) only ever worked for hosts on the
     * local subnet, since you can't ARP a host that isn't on your LAN.
     * Any TCP connection to an address outside the local subnet needs
     * to be sent to the *gateway's* MAC (with the IP header's dst_ip
     * still the real, final destination). */
    uint32_t nh = route_lookup(net_ip, s->dst_ip);
    if (!nh) nh = s->dst_ip;
    uint8_t mac[6];
    int arc = arp_resolve(nh, mac);
    if (arc != 0) return arc;

    /* the opening SYN says what we take: segments of 1460 bytes, and a window counted in 32s */
    int optlen  = flags == TCP_FLAG_SYN ? 8 : 0;
    int tcp_len = 20 + optlen + plen;
    int total   = 14 + 20 + tcp_len;
    uint8_t *pkt = (uint8_t *)malloc(total);
    if (!pkt) return IP_ERR_NOMEM;

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
    *(tcp + 12) = (uint8_t)(((20 + optlen) / 4) << 4);
    *(tcp + 13) = flags;
    {
        /* advertise the real remaining receive space (counted in units of 1 << TCP_RCV_SHIFT once the peer
         * has agreed to scaling; a SYN's own window is never scaled) */
        int w = rx_free(s);
        uint32_t field;
        if (!(flags & TCP_FLAG_SYN) && s->ws_ok) { field = (uint32_t)w >> TCP_RCV_SHIFT; if (field > 65535) field = 65535; s->adv_wnd = (int)(field << TCP_RCV_SHIFT); }
        else { field = w > 65535 ? 65535u : (uint32_t)w; s->adv_wnd = (int)field; }
        *(uint16_t *)(tcp + 14) = htons((uint16_t)field);
    }
    if (optlen) {
        tcp[20] = 2; tcp[21] = 4; tcp[22] = (uint8_t)(TCP_MSS >> 8); tcp[23] = (uint8_t)(TCP_MSS & 0xFF);   /* MSS */
        tcp[24] = 1; tcp[25] = 3; tcp[26] = 3; tcp[27] = TCP_RCV_SHIFT;                                      /* NOP, window scale */
    }
    if (plen > 0) memcpy(tcp + 20 + optlen, payload, plen);
    if (flags & TCP_FLAG_ACK) s->ack_pending = 0;               /* this segment carries the acknowledgement */

    /* Pseudo header + checksum: built and read as plain bytes, never
     * through a mismatched pointer-cast onto this buffer. Doing that
     * (the previous code) was a textbook strict-aliasing violation --
     * writing pseudo[] via a uint32_t-pointer store and then reading
     * it back through a uint16_t-pointer are two incompatible pointer
     * types the compiler is not required to treat as aliasing.
     * Under -O2 this silently dropped the write's visibility to the
     * read for this exact buffer, so every TCP checksum ever sent was
     * wrong -- it just happened to still look internally consistent
     * (fold + complement of some number), so nothing crashed; real
     * receivers just silently discarded the corrupt segment, which is
     * exactly why every TCP SYN got no reply while ICMP (checksummed
     * with ip_checksum()'s byte-indexed style, never hitting this bug)
     * worked fine. */
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(net_ip);         pseudo[1] = (uint8_t)(net_ip >> 8);
    pseudo[2] = (uint8_t)(net_ip >> 16);   pseudo[3] = (uint8_t)(net_ip >> 24);
    pseudo[4] = (uint8_t)(s->dst_ip);      pseudo[5] = (uint8_t)(s->dst_ip >> 8);
    pseudo[6] = (uint8_t)(s->dst_ip >> 16);pseudo[7] = (uint8_t)(s->dst_ip >> 24);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_TCP;
    pseudo[10] = (uint8_t)((unsigned)tcp_len >> 8);
    pseudo[11] = (uint8_t)(tcp_len & 0xFF);

    uint32_t sum = 0;
    int j;
    for (j = 0; j < 12; j += 2) sum += ((uint16_t)pseudo[j] << 8) | pseudo[j + 1];
    sum += csum_net(tcp, tcp_len);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t tcp_cksum = (uint16_t)(~sum & 0xFFFF);
    tcp[16] = (uint8_t)(tcp_cksum >> 8);
    tcp[17] = (uint8_t)(tcp_cksum & 0xFF);

    ip->checksum = 0;
    sum = 0;
    for (j = 0; j < 10; j++) sum += ntohs(((uint16_t *)ip)[j]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->checksum = htons((uint16_t)(~sum & 0xFFFF));

    if (flags & TCP_FLAG_SYN)
        klog_write_hex("tcp: outgoing SYN", pkt, total);

    nic_transmit(pkt, total);
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
            socks[i].peer_mss = TCP_MSS;
            return i;
        }
    }
    return -1;
}

int tcp_connect2(int fd, uint32_t dst_ip, uint16_t dst_port)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_CLOSED) return TCP_ERR_NOSOCK;

    s->dst_ip   = dst_ip;
    s->dst_port = dst_port;
    s->src_port = alloc_port();
    s->seq      = csprng_u32();   /* unpredictable ISN (RFC 6528 intent) */
    s->ack      = 0;
    s->got_rst  = 0;
    s->state    = TCP_SYN_SENT;

    uint32_t syn_seq = s->seq; /* send_seg() bumps s->seq by 1 for the SYN flag on every call -- a resent SYN must carry this same original seq, not a new one */
    int src = send_seg(s, TCP_FLAG_SYN, 0, 0);
    if (src != 0) {
        s->state = TCP_CLOSED;
        return src; /* propagate arp_resolve()'s/IP_ERR_NOMEM verbatim -- see tcp.h */
    }

    /* Wall-clock timeout, not an iteration count — see arp_resolve() for
     * why: how long nic_poll() takes per call varies enormously across
     * drivers/hypervisors, so any fixed retry count is either too short
     * on a slow one or a multi-minute hang on a very slow one. 5 real
     * seconds either way. A single SYN with no retransmission only ever
     * worked against a local/loopback-style path (QEMU slirp) where
     * nothing gets dropped; on a real network hop any one lost SYN
     * burned the entire wait for nothing, which is exactly what made
     * wget fail intermittently once routing actually reached the real
     * internet. Resend at a 1s cadence like a normal TCP stack's
     * initial retransmission timer. */
    uint32_t deadline   = debugmon_uptime_ms() + 5000;
    uint32_t next_retx  = debugmon_uptime_ms() + 1000;
    while (debugmon_uptime_ms() < deadline) {
        uint8_t buf[1536];
        int len = nic_poll(buf, sizeof(buf));
        if (len > 0) {
            /* Log absolutely everything nic_poll() hands back while we
             * wait for the SYN-ACK, not just packets we recognize --
             * this is the only way to tell "nothing at all comes back"
             * apart from "something arrives but our own parsing drops
             * it", which look identical from the shell. */
            klog_write_hex("tcp: packet seen while waiting for SYN-ACK", buf, len);
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(buf, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }
        if (s->state == TCP_ESTABLISHED) return 0;
        if (s->got_rst) { s->state = TCP_CLOSED; return TCP_ERR_REFUSED; }
        if (debugmon_uptime_ms() >= next_retx) {
            s->seq = syn_seq;
            send_seg(s, TCP_FLAG_SYN, 0, 0);
            next_retx += 1000;
        }
        /* Used to task_yield() (int $32) here -- this loop is reachable
         * from a ring3 .t program's blocking tos_net_connect() (SYS_NET_CONNECT,
         * entered via int $0x80), and a nested software interrupt from inside
         * that trap gate's own handler is the same reentrancy bug class
         * confirmed (via a reproducible GPF) in keyboard_getchar()'s own
         * yield -- see kernel/drivers/input/keyboard.c. nic_poll() above
         * already polls the NIC directly, so no yield is needed here either. */
    }
    s->state = TCP_CLOSED;
    return TCP_ERR_TIMEOUT;
}

static void tcp_tick_poll(void);
static void tcp_flush_acks(void);

int tcp_send2(int fd, void *data, int len)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_ESTABLISHED || len < 0) return -1;
    /* send_seg() builds a single frame, so more than one MSS of payload went
     * out as an oversized, unfragmentable packet (TLS records are up to 16 KB).
     * Send MSS-sized segments, and wait for each to be acknowledged before the
     * next because only one segment is kept for retransmission. */
    const uint8_t *p = (const uint8_t *)data;
    int off = 0;
    do {
        int mss = s->peer_mss < TCP_MSS ? s->peer_mss : TCP_MSS;
        int n = len - off < mss ? len - off : mss;
        /* Flow control: never send more than the peer's advertised window leaves
         * after what is still unacknowledged. A zero window is waited out (the peer
         * sends a window update ACK when its application reads); the window used to
         * be ignored completely, so a slow receiver just lost our data. */
        uint32_t wdeadline = debugmon_uptime_ms() + 5000;
        for (;;) {
            uint32_t inflight = s->seq - s->snd_una;
            uint32_t usable = s->snd_wnd > inflight ? s->snd_wnd - inflight : 0;
            if (usable > 0) { if ((uint32_t)n > usable) n = (int)usable; break; }
            if (s->state != TCP_ESTABLISHED || debugmon_uptime_ms() >= wdeadline) return -1;
            tcp_tick_poll();
            uint8_t wpkt[1536];
            int wl = nic_poll(wpkt, sizeof(wpkt));
            if (wl > 0) {
                eth_hdr_t *eth = (eth_hdr_t *)wpkt;
                if (ntohs(eth->type) == ETHERTYPE_ARP) arp_handle(wpkt, wl);
                else if (ntohs(eth->type) == ETHERTYPE_IP)
                    ip_handle(wpkt + sizeof(eth_hdr_t), wl - sizeof(eth_hdr_t));
            }
        }
        uint32_t seq_before = s->seq;
        if (send_seg(s, TCP_FLAG_PSH | TCP_FLAG_ACK, p + off, n) != 0) return -1;
        s->snd_una = seq_before;
        retx_save(s, p + off, n);
        off += n;
        if (off < len) {
            uint32_t deadline = debugmon_uptime_ms() + 5000;
            while (s->retx_len > 0 && s->state == TCP_ESTABLISHED && debugmon_uptime_ms() < deadline) {
                tcp_tick_poll();
                uint8_t pkt[1536];
                int plen = nic_poll(pkt, sizeof(pkt));
                if (plen > 0) {
                    eth_hdr_t *eth = (eth_hdr_t *)pkt;
                    if (ntohs(eth->type) == ETHERTYPE_ARP) arp_handle(pkt, plen);
                    else if (ntohs(eth->type) == ETHERTYPE_IP)
                        ip_handle(pkt + sizeof(eth_hdr_t), plen - sizeof(eth_hdr_t));
                }
            }
            if (s->retx_len > 0) { klog_write("tcp: send gave up, the segment was not acknowledged"); return -1; }   /* peer never acknowledged the segment */
        }
    } while (off < len);
    return 0;
}

/* tcp_tick() had no caller at all, so retransmission (and the half-open
 * reaper) never ran. Drive it from the loops that already poll the NIC,
 * once per 100 ms of wall-clock time. */
static uint32_t tcp_last_tick_ms;
static void tcp_tick_poll(void)
{
    uint32_t now = debugmon_uptime_ms();
    if (now - tcp_last_tick_ms < 100) return;
    tcp_last_tick_ms = now;
    tcp_tick();
}

/* The application has taken data out of the buffer: when that opens the window the peer last saw
 * as closed (or nearly), say so. Without it the peer waits on its zero-window probe timer, which
 * backs off for seconds, and a server that sees a stalled client gives up on the connection. */
static void rx_window_update(tcp_sock_t *s)
{
    if (s->state != TCP_ESTABLISHED && s->state != TCP_FIN_WAIT_1 && s->state != TCP_FIN_WAIT_2) return;
    int w = rx_free(s);
    if (w - s->adv_wnd >= TCP_MSS || (s->adv_wnd < TCP_MSS && w >= TCP_MSS))
        send_seg(s, TCP_FLAG_ACK, 0, 0);
}

int tcp_recv2(int fd, uint8_t *buf, int max_len)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return -1;
    /* Wall-clock timeout, not an iteration count -- see arp_resolve()
     * for why. Without this, a connection that established fine but
     * whose peer never actually sends a reply (or a reply that never
     * makes it back) hung this loop forever -- the shell just sat at
     * "Connecting..." with no way out. Reset on every byte actually
     * received, so a slow-but-alive connection isn't cut off early. */
    uint32_t deadline = debugmon_uptime_ms() + 10000;
    for (;;) {
        if (s->rx_len > 0) {
            int n = rx_take(s, buf, max_len);
            rx_window_update(s);
            return n;
        }
        if (s->rx_closed)           return 0;
        if (s->state == TCP_CLOSED) return -1;
        tcp_flush_acks();
        tcp_tick_poll();
        if (debugmon_uptime_ms() >= deadline) { klog_write("tcp: nothing received for 10 s"); return TCP_ERR_RECV_TIMEOUT; }
        uint8_t pkt[1536];
        int plen = nic_poll(pkt, sizeof(pkt));
        if (plen > 0) {
            deadline = debugmon_uptime_ms() + 10000;
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
    klog_write("tcp: closed by the program (close2)");

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
    rx_release(s);
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
            /* The half-open socket may have been reaped (or never
             * completed the handshake) since it was queued. */
            tcp_sock_t *c = get_sock(new_fd);
            if (!c || c->state != TCP_ESTABLISHED || c->src_port != s->src_port) continue;
            return new_fd;
        }
        tcp_tick_poll();
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
        if (s->used && s->state == TCP_SYN_RECEIVED && ++s->syn_age > TCP_SYN_TIMEOUT) {
            retx_clear(s);
            memset(s, 0, sizeof(tcp_sock_t));
            continue;
        }
        if (!s->used || s->retx_len == 0 || s->state != TCP_ESTABLISHED)
            continue;

        s->retx_tick++;
        if (s->retx_tick < TCP_RETX_TIMEOUT) continue;
        s->retx_tick = 0;
        s->retx_count++;

        if (s->retx_count > TCP_RETX_MAX) {
            klog_write("tcp: the peer stopped acknowledging; connection reset");
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
static int tcp_debug_trace = 0;
void tcp_set_debug_trace(int on) { tcp_debug_trace = on; }

static void tcp_trace_hex32(char *dbg, int *di, uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    for (int sh = 28; sh >= 0; sh -= 4) dbg[(*di)++] = hx[(v >> sh) & 0xF];
}

/* the options of the peer's SYN-ACK: the window scale it agrees to (ours is dropped if it has none) and its MSS */
static void tcp_syn_options(tcp_sock_t *s, const uint8_t *tcp, int hdr_len)
{
    s->ws_ok = 0;
    s->snd_shift = 0;
    for (int i = 20; i < hdr_len;) {
        uint8_t k = tcp[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (i + 1 >= hdr_len) break;
        uint8_t l = tcp[i + 1];
        if (l < 2 || i + l > hdr_len) break;
        if (k == 3 && l == 3) { s->ws_ok = 1; s->snd_shift = tcp[i + 2] > 14 ? 14 : tcp[i + 2]; }
        if (k == 2 && l == 4) { uint16_t m = (uint16_t)((tcp[i + 2] << 8) | tcp[i + 3]); if (m >= 64) s->peer_mss = m; }
        i += l;
    }
}

/* True when the segment's checksum (over pseudo-header + segment) is valid. */
static int tcp_checksum_ok(const ip_hdr_t *ip_hdr, const uint8_t *seg, int len)
{
    uint32_t sum = 0;
    const uint8_t *sp = (const uint8_t *)&ip_hdr->src_ip;
    const uint8_t *dp = (const uint8_t *)&ip_hdr->dst_ip;
    for (int j = 0; j < 4; j += 2) {
        sum += (uint32_t)((sp[j] << 8) | sp[j + 1]);
        sum += (uint32_t)((dp[j] << 8) | dp[j + 1]);
    }
    sum += IPPROTO_TCP;
    sum += (uint32_t)len;
    sum += csum_net(seg, len);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
}

void tcp_handle(ip_hdr_t *ip_hdr, void *pkt, int len)
{
    if (len < 20) return;
    /* data offset below 5 words points inside the fixed header; beyond the
     * segment it points past the data we actually received. */
    {
        int off = (*((uint8_t *)pkt + 12)) >> 4;
        if (off < 5 || off * 4 > len) return;
    }
    if (!tcp_checksum_ok(ip_hdr, (const uint8_t *)pkt, len)) return;

    uint8_t  *tcp      = (uint8_t *)pkt;
    uint16_t  src_port = ntohs(*(uint16_t *)(tcp + 0));
    uint16_t  dst_port = ntohs(*(uint16_t *)(tcp + 2));
    uint32_t  pkt_seq  = ntohl(*(uint32_t *)(tcp + 4));
    uint32_t  pkt_ack  = ntohl(*(uint32_t *)(tcp + 8));
    uint16_t  peer_win = (uint16_t)((tcp[14] << 8) | tcp[15]);
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

    if (tcp_debug_trace) {
        char dbg[100];
        int di = 0;
        const char *lbl = "tcp: dport=";
        while (*lbl) dbg[di++] = *lbl++;
        tcp_trace_hex32(dbg, &di, dst_port);
        const char *l2 = " seq="; while (*l2) dbg[di++] = *l2++;
        tcp_trace_hex32(dbg, &di, pkt_seq);
        const char *l3 = " len="; while (*l3) dbg[di++] = *l3++;
        tcp_trace_hex32(dbg, &di, (uint32_t)data_len);
        const char *l4 = " flags="; while (*l4) dbg[di++] = *l4++;
        tcp_trace_hex32(dbg, &di, flags);
        const char *l5 = " sockfound="; while (*l5) dbg[di++] = *l5++;
        dbg[di++] = s ? '1' : '0';
        if (s) {
            const char *l6 = " s.ack="; while (*l6) dbg[di++] = *l6++;
            tcp_trace_hex32(dbg, &di, s->ack);
        }
        dbg[di++] = '\n'; dbg[di] = 0;
        klog_write(dbg);
    }

    /* LISTEN: incoming SYN -- spawn new socket */
    if (!s && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        for (i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (!socks[i].used) continue;
            if (socks[i].state != TCP_LISTEN) continue;
            if (socks[i].src_port != dst_port) continue;

            /* Backlog full: drop the SYN. The 4-entry accept queue used to
             * be overwritten instead, orphaning the earlier half-open
             * sockets for good and letting a SYN flood use up every
             * socket slot. */
            if (socks[i].accept_tail - socks[i].accept_head >= 4) return;

            int new_fd = tcp_socket();
            if (new_fd < 0) return;
            tcp_sock_t *ns = &socks[new_fd];
            ns->dst_ip   = ip_hdr->src_ip;
            ns->dst_port = src_port;
            ns->src_port = dst_port;
            ns->seq      = csprng_u32();
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
        /* RFC 5961: a reset only counts when it is plausible. In SYN_SENT it
         * must acknowledge our SYN; otherwise its sequence number must be
         * exactly the next one we expect. Anything else (a blind, spoofed
         * RST) is ignored instead of tearing the connection down. */
        if (s->state == TCP_SYN_SENT) {
            if (!(flags & TCP_FLAG_ACK) || pkt_ack != s->seq) return;
        } else if (pkt_seq != s->ack) {
            return;
        }
        klog_write("tcp: reset received from the peer");
        retx_clear(s);
        rx_release(s);
        s->got_rst = 1;
        s->state = TCP_CLOSED;
        return;
    }

    /* RFC 5961 4.2: a SYN on a synchronised connection must not reset it or be
     * processed as data (the old code fell through and accepted its payload);
     * answer with an ACK for what we expect and drop it. This also re-ACKs a
     * retransmitted SYN-ACK whose ACK was lost. */
    if ((flags & TCP_FLAG_SYN) && s->state != TCP_SYN_SENT && s->state != TCP_SYN_RECEIVED) {
        send_seg(s, TCP_FLAG_ACK, 0, 0);
        return;
    }

    /* SYN-ACK (client side) */
    if (s->state == TCP_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            if (pkt_ack != s->seq) return;   /* does not acknowledge our SYN */
            s->ack     = pkt_seq + 1;
            s->seq     = pkt_ack;
            s->snd_una = s->seq;
            s->snd_wnd = peer_win;                 /* a SYN's window is never scaled */
            tcp_syn_options(s, tcp, hdr_len);
            s->state   = TCP_ESTABLISHED;
            send_seg(s, TCP_FLAG_ACK, 0, 0);
        }
        return;
    }

    /* ACK of our SYN-ACK (server side) */
    if (s->state == TCP_SYN_RECEIVED) {
        if ((flags & TCP_FLAG_ACK) && pkt_ack == s->seq) {
            s->snd_una = s->seq;
            s->snd_wnd = peer_win;
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
        /* Only advance (and so acknowledge) what was actually queued: when
         * the buffer is full the segment is dropped unacknowledged and the
         * peer retransmits it after the application has read some. */
        /* acknowledged for every second segment (and by tcp_flush_acks() when the polling round ends), not
         * for each one: an ACK is a frame built, checksummed and sent */
        if (rx_append(s, data, data_len)) {
            if (++s->ack_pending >= 2 || (flags & (TCP_FLAG_FIN | TCP_FLAG_PSH))) send_seg(s, TCP_FLAG_ACK, 0, 0);
        } else {
            send_seg(s, TCP_FLAG_ACK, 0, 0);       /* no room: say what is free */
        }
    } else if (data_len > 0 && SEQ_GT(pkt_seq, s->ack)) {
        /* ahead of a gap: keep it where it belongs, and tell the peer at once what is still missing (its
         * fast retransmit counts these duplicate acknowledgements) */
        rx_hold(s, pkt_seq, data, data_len);
        send_seg(s, TCP_FLAG_ACK, 0, 0);
    } else if (data_len > 0) {
        /* Out-of-sequence data (usually the sender retransmitting a
         * chunk we already ACKed, because our ACK for it never made
         * it back) -- we don't have a reorder buffer so it's dropped
         * either way, but a real TCP always re-sends its current ACK
         * here. Without this, a lost ACK meant the sender kept
         * retransmitting forever while we silently dropped every
         * copy and just... waited, eventually timing out even though
         * the connection was perfectly alive. */
        send_seg(s, TCP_FLAG_ACK, 0, 0);
    }

    /* ACK */
    if (flags & TCP_FLAG_ACK) {
        /* the peer's window travels with every valid ACK (also pure window updates) */
        if (SEQ_LEQ(s->snd_una, pkt_ack) && SEQ_LEQ(pkt_ack, s->seq))
            s->snd_wnd = s->ws_ok ? (uint32_t)peer_win << s->snd_shift : peer_win;
        if (SEQ_GT(pkt_ack, s->snd_una) && SEQ_LEQ(pkt_ack, s->seq)) {
            s->snd_una = pkt_ack;
            if (s->cwnd < s->ssthresh)
                s->cwnd += TCP_MSS;
            else
                s->cwnd += (uint32_t)(TCP_MSS * TCP_MSS) / s->cwnd;
            if (s->retx_len > 0 &&
                SEQ_LEQ(s->retx_seq + (uint32_t)s->retx_len, pkt_ack))
                retx_clear(s);
        }
        if (s->state == TCP_FIN_WAIT_1) s->state = TCP_FIN_WAIT_2;
        if (s->state == TCP_CLOSING)    s->state = TCP_TIME_WAIT;
        if (s->state == TCP_LAST_ACK)   s->state = TCP_CLOSED;
    }

    /* FIN -- only honor it in sequence order, same as the data block
     * above. Without this check, a FIN that arrives even slightly
     * out-of-order relative to trailing data segments (increasingly
     * likely the more segments a transfer needs) closed the connection
     * before all the preceding data had been delivered, silently
     * truncating large downloads. An out-of-order FIN is simply
     * dropped here; the sender's own retransmit timer resends it once
     * the missing segments have had a chance to arrive and be ACKed. */
    /* A FIN rides after the segment's data: its own sequence number is
     * pkt_seq + data_len. After in-order data was accepted above, s->ack has
     * already advanced past it, so comparing against pkt_seq alone made every
     * data+FIN segment (HTTP/1.0 servers send the last bytes and the FIN
     * together) look out of order and the close was never seen. */
    if ((flags & TCP_FLAG_FIN) && pkt_seq + (uint32_t)data_len == s->ack) {
        klog_write("tcp: the peer closed the connection (FIN)");
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
    if (fd < 0) return TCP_ERR_NOSOCK;
    int rc = tcp_connect2(fd, dst_ip, dst_port);
    if (rc != 0) {
        tcp_close2(fd);
        return rc;
    }
    compat_fd = fd;
    return 0;
}

const char *tcp_connect_strerror(int err)
{
    switch (err) {
        case TCP_ERR_NOSOCK:  return "no free TCP socket";
        case TCP_ERR_REFUSED: return "connection refused (RST received)";
        case TCP_ERR_TIMEOUT: return "connection timed out, no reply to SYN";
        case IP_ERR_NOMEM:    return "out of memory building packet";
        default:               return arp_resolve_strerror(err); /* ARP_ERR_* -- couldn't even send the SYN */
    }
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

/* -- Non-blocking use (Linux sockets, kernel/net/lsock.c) ----------------------------------
 * Nothing waits here: the caller asks again later, and tcp_pump() moves the network along
 * in between (epoll and poll call it while they wait). */

/* the data taken in whose acknowledgement is still unsent goes out (the peer must not wait for a second segment) */
static void tcp_flush_acks(void)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_sock_t *s = &socks[i];
        if (s->used && s->ack_pending > 0 &&
            (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2 || s->state == TCP_CLOSE_WAIT))
            send_seg(s, TCP_FLAG_ACK, 0, 0);
    }
}

void tcp_pump(void)
{
    for (int i = 0; i < 1024; i++) {
        if (!net_poll()) break;
        if ((i & 63) == 63) tcp_flush_acks();            /* a long burst: not one acknowledgement at its end only */
    }
    tcp_flush_acks();
    tcp_tick_poll();
}

int tcp_connect_begin(int fd, uint32_t dst_ip, uint16_t dst_port)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s || s->state != TCP_CLOSED) return TCP_ERR_NOSOCK;
    s->dst_ip   = dst_ip;
    s->dst_port = dst_port;
    s->src_port = alloc_port();
    s->seq      = csprng_u32();
    s->ack      = 0;
    s->got_rst  = 0;
    s->state    = TCP_SYN_SENT;
    s->syn_seq  = s->seq;
    int r = send_seg(s, TCP_FLAG_SYN, 0, 0);
    if (r != 0) { s->state = TCP_CLOSED; return r; }
    s->syn_next_ms = debugmon_uptime_ms() + 1000;
    s->syn_deadline_ms = debugmon_uptime_ms() + 20000;
    return 0;
}

/* 1 connected, 0 still connecting (the SYN is resent every second), or TCP_ERR_* */
int tcp_connect_poll(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return TCP_ERR_NOSOCK;
    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) return 1;
    if (s->got_rst) { s->state = TCP_CLOSED; return TCP_ERR_REFUSED; }
    if (s->state != TCP_SYN_SENT) return TCP_ERR_REFUSED;
    uint32_t now = debugmon_uptime_ms();
    if (now >= s->syn_deadline_ms) { s->state = TCP_CLOSED; return TCP_ERR_TIMEOUT; }
    if (now >= s->syn_next_ms) {
        s->seq = s->syn_seq;
        send_seg(s, TCP_FLAG_SYN, 0, 0);
        s->syn_next_ms = now + 1000;
    }
    return 0;
}

/* bytes waiting to be read, or -1 when the peer has finished (or the connection is gone) */
int tcp_rx_ready(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return -1;
    if (s->rx_len > 0) return s->rx_len;
    if (s->rx_closed || s->state == TCP_CLOSED || s->got_rst) return -1;
    return 0;
}

/* what is there now: >0 bytes, 0 the end, TCP_ERR_RECV_TIMEOUT nothing yet */
int tcp_recv_nb(int fd, uint8_t *buf, int max_len)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) { klog_write("tcp: recv on a socket that is gone (end of file)"); return 0; }
    if (s->rx_len > 0) {
        int n = rx_take(s, buf, max_len);
        rx_window_update(s);
        return n;
    }
    if (s->rx_closed || s->state == TCP_CLOSED || s->got_rst) {
        klog_write(s->got_rst ? "tcp: end of file: reset" : s->rx_closed ? "tcp: end of file: peer FIN" : "tcp: end of file: socket closed");
        return 0;
    }
    return TCP_ERR_RECV_TIMEOUT;
}

int tcp_is_established(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    return s && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT);
}

void tcp_local(int fd, uint16_t *port)
{
    tcp_sock_t *s = get_sock(fd);
    *port = s ? s->src_port : 0;
}

/* gone at once, without waiting for the peer's FIN: what an RST-less close of a socket the
 * program has finished with needs (tcp_close2() waits up to seconds for the goodbye) */
void tcp_release(int fd)
{
    tcp_sock_t *s = get_sock(fd);
    if (!s) return;
    klog_write("tcp: closed by the program (release)");
    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        send_seg(s, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        tcp_pump();
    }
    if (s->rx_buf) free(s->rx_buf);
    if (s->retx_data) free(s->retx_data);
    memset(s, 0, sizeof(*s));
}
