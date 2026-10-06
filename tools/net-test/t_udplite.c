/* Host test for kernel/net/udplite.c against real Linux UDP-Lite datagrams (vectors/udplite_linux.hex). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
#include "nic.h"
void (*nic_send)(void *, int);
int (*nic_poll)(uint8_t *, int);
uint8_t net_mac[6]; uint32_t net_ip, net_gateway, net_dns, net_netmask, nic_tx_packets, nic_tx_bytes, fake_now_ms;
static uint8_t sent[4][2048]; static int sent_len[4]; static int nsent, fail_ip_send;
int ip_send(uint32_t dst, uint8_t proto, void *data, int len)
{
    (void)dst; (void)proto;
    if (fail_ip_send) return -1;
    if (nsent < 4 && len <= 2048) { memcpy(sent[nsent], data, len); sent_len[nsent] = len; nsent++; }
    return 0;
}
void arp_handle(uint8_t *d, int l) { (void)d; (void)l; }
void ip_handle(uint8_t *d, int l) { (void)d; (void)l; }
static int fail_malloc;
static void *test_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
#define malloc test_malloc
#include "udplite.c"
#undef malloc

static uint8_t pk[4][1500]; static int pklen[4];
static int bad;
static void check(const char *label, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", label); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int deliver(uint8_t *p, int n, uint8_t *out, int cap, int *got)
{
    /* handle, then read back what the socket queued */
    udplite_handle((ip_hdr_t *)p, p + 20, n - 20);
    if (!sockets[0].has_data) { *got = 0; return 0; }
    int m = sockets[0].len < cap ? sockets[0].len : cap;
    memcpy(out, sockets[0].data, m); *got = m;
    free(sockets[0].data); sockets[0].data = 0; sockets[0].has_data = 0; sockets[0].len = 0;
    return 1;
}
int main(void)
{
    FILE *f = fopen("vectors/udplite_linux.hex", "r");
    char *line = malloc(8192); int n = 0;
    while (n < 4 && fgets(line, 8192, f)) {
        if (line[0] == '#' || strlen(line) < 20) continue;
        int l = (int)strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        pklen[n] = l / 2;
        for (int i = 0; i < pklen[n]; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); pk[n][i] = (uint8_t)v; }
        n++;
    }
    uint32_t src, dst; memcpy(&src, pk[0] + 12, 4); memcpy(&dst, pk[0] + 16, 4);
    net_ip = dst;
    udplite_open(5003);

    /* the checksum function reproduces Linux's for full, partial, header-only and odd-length datagrams */
    int ck = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t t[1500]; int l = pklen[i] - 20; memcpy(t, pk[i] + 20, l);
        uint16_t cov = get16(t + 4), stored = get16(t + 6);
        put16(t + 6, 0);
        uint16_t c = ntohs(udplite_checksum(src, dst, t, l, cov == l ? 0 : cov));
        if (c != stored) { ck = 0; printf("   packet %d: ours %04x, Linux %04x\n", i, c, stored); }
    }
    check("udplite_checksum() reproduces Linux's checksums (full, partial 12, header-only, odd length)", ck);

    uint8_t out[1500]; int got;
    check("full-coverage datagram delivered", deliver(pk[0], pklen[0], out, sizeof(out), &got) && got == 22 && memcmp(out, "full-coverage-datagram", 22) == 0);
    check("partial-coverage datagram delivered", deliver(pk[1], pklen[1], out, sizeof(out), &got) && got == 24);
    check("header-only coverage delivered", deliver(pk[2], pklen[2], out, sizeof(out), &got) && got == 19);
    check("odd-length datagram delivered", deliver(pk[3], pklen[3], out, sizeof(out), &got) && got == 33);

    /* the point of UDP-Lite: damage outside the covered bytes is tolerated, inside is not */
    uint8_t t[1500];
    memcpy(t, pk[1], pklen[1]); t[20 + 8 + 10] ^= 0xFF;                       /* payload byte 10: beyond coverage 12 */
    check("corruption outside the covered bytes is tolerated", deliver(t, pklen[1], out, sizeof(out), &got));
    memcpy(t, pk[1], pklen[1]); t[20 + 8 + 2] ^= 0x01;                        /* inside the covered 12 bytes */
    check("corruption inside the covered bytes is dropped", !deliver(t, pklen[1], out, sizeof(out), &got));
    memcpy(t, pk[0], pklen[0]); t[pklen[0] - 1] ^= 0x01;                      /* full coverage: any bit counts */
    check("full coverage: one flipped payload bit is dropped", !deliver(t, pklen[0], out, sizeof(out), &got));
    memcpy(t, pk[0], pklen[0]); put16(t + 20 + 6, 0);
    check("checksum 0 is illegal in UDP-Lite and dropped", !deliver(t, pklen[0], out, sizeof(out), &got));
    memcpy(t, pk[0], pklen[0]); put16(t + 20 + 4, 5);
    check("coverage 5 (1-7 are illegal) is dropped", !deliver(t, pklen[0], out, sizeof(out), &got));
    memcpy(t, pk[0], pklen[0]); put16(t + 20 + 4, (uint16_t)(pklen[0] - 20 + 40));
    check("coverage larger than the datagram is dropped", !deliver(t, pklen[0], out, sizeof(out), &got));
    memcpy(t, pk[0], pklen[0]); memset(t + 20 + 4, 0, 2); put16(t + 20 + 6, 0);
    {   /* coverage 0 = whole datagram: valid when the checksum is */
        uint8_t u[1500]; int l = pklen[0] - 20; memcpy(u, t + 20, l); put16(u + 6, 0);
        put16(t + 20 + 6, ntohs(udplite_checksum(src, dst, u, l, 0)));
        check("coverage field 0 (= whole datagram) with a valid checksum is accepted", deliver(t, pklen[0], out, sizeof(out), &got));
    }

    /* malloc failure must not leave a dangling 'has_data' */
    fail_malloc = 1;
    udplite_handle((ip_hdr_t *)pk[0], pk[0] + 20, pklen[0] - 20);
    fail_malloc = 0;
    check("allocation failure leaves no queued datagram with a NULL buffer", !(sockets[0].has_data && !sockets[0].data));
    sockets[0].has_data = 0;

    /* an unread datagram is not replaced by (or lost to) a failed allocation */
    deliver(pk[0], pklen[0], out, sizeof(out), &got);       /* leave it queued? deliver consumed it */
    udplite_handle((ip_hdr_t *)pk[0], pk[0] + 20, pklen[0] - 20);        /* queue one */
    fail_malloc = 1; udplite_handle((ip_hdr_t *)pk[2], pk[2] + 20, pklen[2] - 20); fail_malloc = 0;
    check("a failed allocation keeps the datagram that was already queued intact", sockets[0].has_data && sockets[0].data && sockets[0].len == 22);
    if (sockets[0].data) { free(sockets[0].data); sockets[0].data = 0; } sockets[0].has_data = 0;

    /* send: bad lengths, coverage normalisation, valid result */
    nsent = 0;
    static uint8_t big[4000]; memset(big, 'a', sizeof(big));
    check("oversized datagram is refused", udplite_send(dst, 5003, 6000, big, 3000, 0) != 0 && nsent == 0);
    check("negative length is refused", udplite_send(dst, 5003, 6000, big, -1, 0) != 0 && nsent == 0);
    check("a normal send goes out", udplite_send(dst, 5003, 6000, "hello", 5, 8) == 0 && nsent == 1);
    {
        uint8_t u[2048]; int l = sent_len[0]; memcpy(u, sent[0], l); uint16_t st = get16(u + 6); put16(u + 6, 0);
        check("our datagram's checksum is right (coverage 8)", ntohs(udplite_checksum(net_ip, dst, u, l, 8)) == st && get16(u + 4) == 8);
    }
    nsent = 0; udplite_send(dst, 5003, 6000, "hello", 5, 5000);
    check("coverage above the datagram length is clamped to the datagram (not sent as an invalid value)", nsent == 1 && get16(sent[0] + 4) <= sent_len[0] && get16(sent[0] + 4) >= 8);
    printf("failures: %d\n", bad);
    return bad != 0;
}
