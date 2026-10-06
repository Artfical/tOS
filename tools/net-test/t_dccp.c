/* Host test for kernel/net/dccp.c against a real Linux DCCP association (vectors/dccp_linux.hex). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
#include "nic.h"
void (*nic_send)(void *, int);
int (*nic_poll)(uint8_t *, int);
uint8_t net_mac[6]; uint32_t fake_now_ms; void arp_handle(uint8_t *d, int l) { (void)d; (void)l; }
uint32_t net_ip, net_gateway, net_dns, net_netmask, nic_tx_packets, nic_tx_bytes;
static uint8_t sent[16][2048]; static int sent_len[16]; static int nsent;
static int scripted_mode, peer_silent, drop_first_request, pending_response, requests_sent;
int ip_send(uint32_t dst, uint8_t proto, void *data, int len)
{
    (void)dst; (void)proto;
    if (nsent < 16 && len <= 2048) { memcpy(sent[nsent], data, len); sent_len[nsent] = len; nsent++; }
    if (scripted_mode && len >= 9 && (((uint8_t *)data)[8] >> 1 & 15) == 0) {
        requests_sent++;
        if (!peer_silent && !(drop_first_request && requests_sent == 1)) pending_response = 1;
    }
    return 0;
}
uint32_t csprng_u32(void) { static uint32_t x = 0x9E3779B9; x = x * 1664525 + 1013904223; return x; }
void csprng_fill(uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)csprng_u32(); }
#include "dccp.c"

static uint8_t pk[8][1500]; static int pklen[8];
static int bad;
static void check(const char *label, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", label); }
static void feed(uint8_t *p, int n) { dccp_handle((ip_hdr_t *)p, p + 20, n - 20); }
static void fix_checksum(uint8_t *ip_pkt, int n)
{
    uint8_t *s = ip_pkt + 20; put16(s + 6, 0);
    put16(s + 6, dccp_checksum(*(uint32_t *)(ip_pkt + 12), *(uint32_t *)(ip_pkt + 16), s, n - 20));
}
static int cksum_ok(const uint8_t *seg, int len, uint32_t src, uint32_t dst)
{
    uint8_t c[2048]; memcpy(c, seg, len); put16(c + 6, 0);
    return dccp_checksum(src, dst, c, len) == get16(seg + 6);
}
static int ptype(const uint8_t *s) { return (s[8] >> 1) & 15; }
static uint32_t get32_fn(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
/* scripted peer: every poll costs 100 ms; a pending Response is the real one patched to the live connection */
static int scripted_poll(uint8_t *buf, int max)
{
    (void)max;
    fake_now_ms += 100;
    if (!pending_response) return 0;
    pending_response = 0;
    memset(buf, 0, 1536);
    memcpy(buf + 14, pk[1], pklen[1]);
    buf[12] = 0x08; buf[13] = 0x00;
    uint8_t *ip = buf + 14, *s = ip + 20;
    put16(s + 0, dconn.dst_port); put16(s + 2, dconn.src_port);
    put48(s + 10, 777777);                                        /* the server's sequence number */
    put48(s + 18, dconn.gss);                                     /* acknowledges our latest Request */
    memcpy(ip + 12, &dconn.dst_ip, 4); memcpy(ip + 16, &net_ip, 4);
    ip[10] = ip[11] = 0;
    uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += (uint32_t)((ip[i] << 8) | ip[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    put16(ip + 10, (uint16_t)~sum);
    put16(s + 6, 0); put16(s + 6, dccp_checksum(dconn.dst_ip, net_ip, s, pklen[1] - 20));
    return 14 + pklen[1];
}

int main(void)
{
    FILE *f = fopen("vectors/dccp_linux.hex", "r");
    char *line = malloc(8192); int n = 0;
    while (n < 8 && fgets(line, 8192, f)) {
        if (line[0] == '#' || strlen(line) < 20) continue;
        int l = (int)strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        pklen[n] = l / 2;
        for (int i = 0; i < pklen[n]; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); pk[n][i] = (uint8_t)v; }
        n++;
    }
    uint32_t cip = *(uint32_t *)(pk[0] + 12), sip = *(uint32_t *)(pk[0] + 16);
    uint16_t cport = get16(pk[0] + 20), sport = get16(pk[0] + 22);
    uint64_t iss = get48(pk[0] + 20 + 10);

    int ck = 1;
    for (int i = 0; i < 8; i++) ck &= cksum_ok(pk[i] + 20, pklen[i] - 20, *(uint32_t *)(pk[i] + 12), *(uint32_t *)(pk[i] + 16));
    check("dccp_checksum() reproduces the checksum of all 8 real Linux packets", ck);

    /* white-box: the client as it was when the capture started */
    net_ip = cip;
    memset(&dconn, 0, sizeof(dconn));
    dconn.dst_ip = sip; dconn.dst_port = sport; dconn.src_port = cport; dconn.service = 42;
    dconn.iss = iss; dconn.gss = iss; dconn.state = DCCP_STATE_REQUEST; dconn_active = 1;

    /* our Request: X=1, service code, valid checksum */
    {
        uint64_t saved = dconn.gss; dconn.gss = (iss - 1) & DCCP_SEQ_MASK; nsent = 0;
        send_request();
        const uint8_t *s = sent[0];
        check("Request: type 0, X=1, offset 5 words, service code 42, seq = iss, valid checksum",
              nsent == 1 && ptype(s) == 0 && (s[8] & 1) && s[4] == 5 && get32_fn(s + 16) == 42 && get48(s + 10) == iss && cksum_ok(s, sent_len[0], cip, sip));
        dconn.gss = saved;
    }

    /* real Response: accepted, Ack sent with the right number */
    nsent = 0; feed(pk[1], pklen[1]);
    check("real Response accepted: connection OPEN", dconn.state == DCCP_STATE_OPEN);
    {
        const uint8_t *s = sent[0];
        uint64_t rseq = get48(pk[1] + 20 + 10);
        check("our Ack: type 3, X=1, 6 words, ack = the Response's seq, seq = iss+1, valid checksum, right ports",
              nsent == 1 && ptype(s) == 3 && (s[8] & 1) && s[4] == 6 && get48(s + 18) == rseq && get48(s + 10) == seq_add(iss, 1) &&
              cksum_ok(s, sent_len[0], cip, sip) && get16(s) == cport && get16(s + 2) == sport);
    }

    /* our Data, then the real DataAck from the server */
    nsent = 0; dccp_send("hello-dccp", 10);
    {
        const uint8_t *s = sent[0];
        check("our Data: type 2, X=1, 16-byte header, payload, valid checksum",
              nsent == 1 && ptype(s) == 2 && s[4] == 4 && memcmp(s + 16, "hello-dccp", 10) == 0 && cksum_ok(s, sent_len[0], cip, sip));
    }
    feed(pk[4], pklen[4]);
    check("real DataAck delivers 'world-from-linux-dccp'", dconn.rx_len == 21 && memcmp(dconn.rx_buf, "world-from-linux-dccp", 21) == 0);

    /* negative cases against the open connection */
    {
        uint8_t t[1500]; int before = dconn.rx_len;
        memcpy(t, pk[4], pklen[4]); t[20 + 22 - 20 + 0] ^= 0;      /* (unchanged copy) */
        memcpy(t, pk[4], pklen[4]); t[20 + 2] ^= 1;                  /* wrong destination port */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("wrong destination port is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); t[20 + 6] ^= 1;                  /* corrupted checksum */
        feed(t, pklen[4]);
        check("bad checksum is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); *(uint32_t *)(t + 12) ^= 0x01000000u;   /* from another host */
        feed(t, pklen[4]);
        check("packet from another host is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); t[20 + 4] = 3;                   /* offset too small for a DataAck (needs 6 words) */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("data offset smaller than the packet type needs is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); t[20 + 4] = 200;                 /* offset beyond the packet */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("data offset beyond the packet is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); t[20 + 8] = (uint8_t)((11 << 1) | 1);   /* reserved packet type */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("reserved packet type is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); t[20 + 5] = 0x03;                /* CsCov != 0 (partial checksum) */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("partial checksum coverage is refused", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); put48(t + 20 + 10, seq_add(dconn.gsr, 5000));   /* far outside the sequence window */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("sequence number outside the validity window is ignored", dconn.rx_len == before);
        memcpy(t, pk[4], pklen[4]); put48(t + 20 + 18, seq_add(dconn.gss, 1000));   /* acknowledges something we never sent */
        fix_checksum(t, pklen[4]); feed(t, pklen[4]);
        check("acknowledgement of a packet we never sent is ignored", dconn.rx_len == before);
        memcpy(t, pk[0], pklen[0]); feed(t, pklen[0]);               /* a Request aimed at a client */
        check("a Request packet is ignored by the client", dconn.rx_len == before && dconn.state == DCCP_STATE_OPEN);
    }

    /* orderly close: CloseReq -> we send Close -> peer's Reset */
    nsent = 0; feed(pk[5], pklen[5]);
    {
        const uint8_t *s = sent[0];
        check("CloseReq answered with Close (type 6, ack = CloseReq seq)", nsent == 1 && ptype(s) == 6 && get48(s + 18) == get48(pk[5] + 20 + 10) && dconn.state == DCCP_STATE_CLOSING);
    }
    feed(pk[7], pklen[7]);
    check("Reset ends the connection", dconn.state == DCCP_STATE_CLOSED && dconn.rx_closed);

    /* Close from the peer: answered with Reset (code 1), never a Reset for a Reset */
    memset(&dconn, 0, sizeof(dconn));
    dconn.dst_ip = sip; dconn.dst_port = sport; dconn.src_port = cport; dconn.state = DCCP_STATE_OPEN; dconn_active = 1;
    dconn.gss = get48(pk[6] + 20 + 10) - 1 + 0; dconn.gss = seq_add(get48(pk[5] + 20 + 18), 0); dconn.gsr = get48(pk[5] + 20 + 10) - 1; dconn.have_gsr = 1;
    nsent = 0; feed(pk[5], pklen[5]); nsent = 0;
    dconn.state = DCCP_STATE_OPEN; dconn.rx_closed = 0;
    {   /* build a Close from the server by retyping the CloseReq vector */
        uint8_t t[1500]; memcpy(t, pk[5], pklen[5]); t[20 + 8] = (uint8_t)((DCCP_PKT_CLOSE << 1) | 1); put48(t + 20 + 10, seq_add(dconn.gsr, 1));
        fix_checksum(t, pklen[5]); feed(t, pklen[5]);
        check("peer's Close is answered with a Reset (code 'Closed') and ends the connection",
              nsent == 1 && ptype(sent[0]) == 7 && sent[0][16 + 8] == 1 && dconn.state == DCCP_STATE_CLOSED);
        nsent = 0; dconn.state = DCCP_STATE_OPEN;
        uint8_t u[1500]; memcpy(u, pk[7], pklen[7]); put48(u + 20 + 10, seq_add(dconn.gsr, 1)); put48(u + 20 + 18, dconn.gss); fix_checksum(u, pklen[7]); feed(u, pklen[7]);
        check("a Reset is never answered", nsent == 0 && dconn.state == DCCP_STATE_CLOSED);
    }

    /* connect(): wall-clock timeout, Request retransmission, random port/ISN */
    {
        scripted_mode = 1; nic_poll = scripted_poll; net_ip = cip;
        dconn_active = 0; requests_sent = 0; pending_response = 0; peer_silent = 0; drop_first_request = 1; fake_now_ms = 1000;
        int rc = dccp_connect_service(sip, 5002, 42);
        printf("   connect with a lost first Request: rc=%d, Requests sent=%d, elapsed fake ms=%u\n", rc, requests_sent, fake_now_ms - 1000);
        check("a lost Request is retransmitted and the connection comes up", rc == 0 && requests_sent == 2 && dconn.state == DCCP_STATE_OPEN);
        check("source port is random in 49152-65535 and the ISN is not the constant 1", dconn.src_port >= 49152 && dconn.iss > 1000);
        dccp_close();
        dconn_active = 0; requests_sent = 0; peer_silent = 1; fake_now_ms = 1000;
        rc = dccp_connect_service(sip, 5002, 42);
        printf("   connect to a silent peer: rc=%d, Requests sent=%d, elapsed fake ms=%u\n", rc, requests_sent, fake_now_ms - 1000);
        check("a silent peer: gives up after about 5 s", rc == -1 && fake_now_ms - 1000 >= 5000 && fake_now_ms - 1000 < 6000 && requests_sent >= 4);
        memset(&dconn, 0, sizeof(dconn)); dconn.state = DCCP_STATE_OPEN; dconn_active = 1; fake_now_ms = 1000; uint8_t rb[32];
        rc = dccp_recv(rb, sizeof(rb));
        check("dccp_recv times out after ~10 s of silence", rc == DCCP_ERR_TIMEOUT && fake_now_ms - 1000 >= 10000 && fake_now_ms - 1000 < 10500);
    }
    printf("failures: %d\n", bad);
    return bad != 0;
}
