/* Host harness for kernel/net/tcp.c: crafted segments in, captured frames out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "nic.h"
uint8_t net_mac[6] = {0x52,0x54,0,0x12,0x34,0x56}; uint32_t net_ip, net_gateway, net_dns, net_netmask;
uint32_t fake_now_ms;
void (*nic_send)(void *, int); int (*nic_poll)(uint8_t *, int);
static uint8_t frames[64][1600]; static int frame_len[64]; static int nframes;
void nic_transmit(void *d, int l) { if (nframes < 64 && l <= 1600) { memcpy(frames[nframes], d, l); frame_len[nframes] = l; nframes++; } }
int arp_resolve(uint32_t ip, uint8_t *mac) { (void)ip; memset(mac, 0xAA, 6); return 0; }
void arp_handle(uint8_t *d, int l) { (void)d; (void)l; }
const char *arp_resolve_strerror(int e) { (void)e; return "e"; }
uint32_t route_lookup(uint32_t s, uint32_t d) { (void)s; return d; }
void ip_handle(uint8_t *d, int l) { (void)d; (void)l; }
void klog_write_hex(const char *s, const void *d, int l) { (void)s; (void)d; (void)l; }
void *krealloc(void *p, size_t n) { return realloc(p, n); }
uint32_t csprng_u32(void) { static uint32_t x = 0xA5A5A5A5; x = x * 1664525 + 1013904223; return x; }
void csprng_fill(uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)csprng_u32(); }
static int test_poll(uint8_t *b, int m) { (void)b; (void)m; fake_now_ms += 100; return 0; }
#include "tcp.c"

#define PEER IP4(10,0,2,2)
static int bad;
static void check(const char *l, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", l); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* build an IP packet + TCP segment from the peer to us and hand it to tcp_handle() */
static void inject(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                   const void *data, int dlen)
{
    uint8_t p[1600]; memset(p, 0, sizeof(p));
    ip_hdr_t *ip = (ip_hdr_t *)p; ip->ver_ihl = 0x45; ip->protocol = IPPROTO_TCP; ip->src_ip = PEER; ip->dst_ip = net_ip;
    uint8_t *t = p + 20; int tl = 20 + dlen;
    t[0] = sport >> 8; t[1] = (uint8_t)sport; t[2] = dport >> 8; t[3] = (uint8_t)dport;
    put32(t + 4, seq); put32(t + 8, ack); t[12] = 0x50; t[13] = flags; t[14] = win >> 8; t[15] = (uint8_t)win;
    if (dlen) memcpy(t + 20, data, dlen);
    uint32_t sum = 0; const uint8_t *sp = (uint8_t *)&ip->src_ip, *dp = (uint8_t *)&ip->dst_ip;
    sum += (sp[0] << 8) | sp[1]; sum += (sp[2] << 8) | sp[3]; sum += (dp[0] << 8) | dp[1]; sum += (dp[2] << 8) | dp[3];
    sum += IPPROTO_TCP; sum += tl;
    for (int i = 0; i + 1 < tl; i += 2) sum += (t[i] << 8) | t[i + 1];
    if (tl & 1) sum += t[tl - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = (uint16_t)~sum; t[16] = c >> 8; t[17] = (uint8_t)c;
    tcp_handle(ip, t, tl);
}
/* the TCP segment inside captured frame i */
static const uint8_t *seg(int i) { return frames[i] + 14 + 20; }

int main(void)
{
    net_ip = IP4(10,0,2,15);
    nic_poll = test_poll;
    memset(socks, 0, sizeof(socks));

    /* ---- a connected client socket, set up white-box as if the handshake had completed ---- */
    int fd = tcp_socket();
    tcp_sock_t *s = &socks[fd];
    s->dst_ip = PEER; s->dst_port = 80; s->src_port = 50000;
    s->seq = 1000; s->snd_una = 1000; s->ack = 5000; s->state = TCP_ESTABLISHED;

    /* data in order is accepted and acknowledged */
    nframes = 0; inject(80, 50000, 5000, 1000, TCP_FLAG_ACK | TCP_FLAG_PSH, 8192, "hello", 5);
    check("in-order data accepted (ack advances by 5) and ACKed", s->ack == 5005 && s->rx_len == 5 && nframes == 1 && be32(seg(0) + 8) == 5005);

    /* ---- SYN on an established connection: answered with an ACK (RFC 5961 4.2), never processed as data ---- */
    nframes = 0; uint32_t ack_before = s->ack; int rx_before = s->rx_len;
    inject(80, 50000, 5005, 1000, TCP_FLAG_SYN, 8192, "XXXX", 4);
    check("SYN (with data) on an established connection: ACK sent, data not accepted, state intact",
          nframes == 1 && (seg(0)[13] & TCP_FLAG_ACK) && s->ack == ack_before && s->rx_len == rx_before && s->state == TCP_ESTABLISHED);

    /* ---- data + FIN in one segment closes the receive side ---- */
    s->ack = 5005; s->rx_len = 0; if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }
    nframes = 0; inject(80, 50000, 5005, 1000, TCP_FLAG_ACK | TCP_FLAG_FIN | TCP_FLAG_PSH, 8192, "bye", 3);
    check("data+FIN: data delivered, FIN consumed, CLOSE_WAIT", s->rx_len == 3 && s->rx_closed && s->state == TCP_CLOSE_WAIT && s->ack == 5005 + 3 + 1);

    /* ---- segment checks ---- */
    s->state = TCP_ESTABLISHED; s->rx_closed = 0; s->rx_len = 0; if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }
    uint32_t a0 = s->ack;
    nframes = 0; inject(80, 50000, a0 + 100, 1000, TCP_FLAG_ACK, 8192, "later", 5);
    check("out-of-order data is not accepted; a duplicate ACK for what we expect is sent", s->ack == a0 && s->rx_len == 0 && nframes == 1 && be32(seg(0) + 8) == a0);
    nframes = 0; inject(80, 50000, a0 - 5, 1000, TCP_FLAG_ACK, 8192, "old!!", 5);
    check("old (already acknowledged) data is not accepted twice", s->ack == a0 && s->rx_len == 0);

    /* ---- ACKs beyond what was sent are ignored ---- */
    uint32_t una0 = s->snd_una;
    inject(80, 50000, a0, s->seq + 5000, TCP_FLAG_ACK, 8192, 0, 0);
    check("an ACK for data we never sent does not move snd_una", s->snd_una == una0);

    /* ---- receive buffer cap and zero window ---- */
    s->rx_len = 0; if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }
    {
        static uint8_t chunk[1400]; memset(chunk, 'z', sizeof(chunk));
        uint32_t sq = s->ack; int accepted = 0;
        for (int i = 0; i < 60; i++) {                                 /* 84000 bytes offered, nobody reads */
            uint32_t before = s->ack;
            nframes = 0; inject(80, 50000, sq, 1000, TCP_FLAG_ACK, 8192, chunk, sizeof(chunk));
            if (s->ack != before) { accepted += sizeof(chunk); sq = s->ack; }
        }
        printf("   accepted %d bytes of 84000 offered, rx_len=%d\n", accepted, s->rx_len);
        check("receive buffer stops at its cap (data beyond it is not acknowledged)", s->rx_len <= TCP_RX_MAX && accepted <= TCP_RX_MAX);
        int adv = (seg(nframes - 1)[14] << 8) | seg(nframes - 1)[15];
        printf("   window advertised with a full buffer: %d\n", adv);
        check("the advertised window has shrunk to less than one segment when the buffer is nearly full", nframes >= 1 && adv < TCP_MSS);
    }
    s->rx_len = 0; if (s->rx_buf) { free(s->rx_buf); s->rx_buf = 0; }

    /* ---- peer window: nothing is sent into a closed window ---- */
    s->state = TCP_ESTABLISHED; s->rx_closed = 0; s->snd_una = s->seq;
    inject(80, 50000, s->ack, s->seq, TCP_FLAG_ACK, 0, 0, 0);          /* zero window advertised */
    check("the zero window is recorded", s->snd_wnd == 0);
    nframes = 0; fake_now_ms = 100000;
    int wr = tcp_send2(fd, "0123456789", 10);
    check("send into a zero window is not transmitted (and fails after the timeout)", nframes == 0 && wr != 0);

    /* ---- listener: backlog and SYN flood ---- */
    {
        int lfd = tcp_socket(); tcp_listen(lfd, 8080);
        int before_used = 0; for (int i = 0; i < TCP_MAX_SOCKETS; i++) before_used += socks[i].used;
        for (int i = 0; i < 12; i++) inject((uint16_t)(30000 + i), 8080, 7000 + i, 0, TCP_FLAG_SYN, 1000, 0, 0);
        int after_used = 0; for (int i = 0; i < TCP_MAX_SOCKETS; i++) after_used += socks[i].used;
        printf("   sockets before %d after 12 SYNs %d\n", before_used, after_used);
        check("a SYN flood creates at most 4 half-open sockets (the accept backlog)", after_used - before_used <= 4);
        for (int t = 0; t <= TCP_SYN_TIMEOUT + 1; t++) tcp_tick();
        int reaped = 0; for (int i = 0; i < TCP_MAX_SOCKETS; i++) reaped += socks[i].used;
        check("half-open sockets are reaped after the SYN timeout", reaped == before_used);
    }
    printf("failures: %d\n", bad);
    return bad != 0;
}
