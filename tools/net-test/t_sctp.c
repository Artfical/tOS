/* Host test for kernel/net/sctp.c against a real Linux SCTP association (vectors/sctp_linux.hex). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
#include "nic.h"
void (*nic_send)(void *, int);
int (*nic_poll)(uint8_t *, int);
uint8_t net_mac[6]; uint32_t net_ip, net_gateway, net_dns, net_netmask;
uint32_t nic_tx_packets, nic_tx_bytes;
/* capture what sctp.c sends */
static uint8_t sent[8][2048]; static int sent_len[8]; static int nsent;
int ip_send(uint32_t dst, uint8_t proto, void *data, int len)
{
    (void)dst; (void)proto;
    if (nsent < 8 && len <= 2048) { memcpy(sent[nsent], data, len); sent_len[nsent] = len; nsent++; }
    return 0;
}
uint32_t csprng_u32(void) { static uint32_t x = 0x12345678; x = x * 1664525 + 1013904223; return x; }
#include "sctp.c"

static uint8_t pk[11][1500]; static int pklen[11]; /* IP packets */
static int bad;
static void check(const char *label, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", label); }
static void feed(int i)   /* deliver packet i (IP header + SCTP) to the handler */
{
    ip_hdr_t *ip = (ip_hdr_t *)pk[i];
    sctp_handle(ip, pk[i] + 20, pklen[i] - 20);
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int main(void)
{
    FILE *f = fopen("vectors/sctp_linux.hex", "r");
    char *line = malloc(8192); int n = 0;
    while (n < 11 && fgets(line, 8192, f)) {
        if (line[0] == '#' || strlen(line) < 20) continue;
        int l = (int)strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        pklen[n] = l / 2;
        for (int i = 0; i < pklen[n]; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); pk[n][i] = (uint8_t)v; }
        n++;
    }
    /* the client's state as it was when the capture was made (white-box) */
    const uint8_t *init = pk[0] + 20;
    uint16_t cport = (uint16_t)((init[0] << 8) | init[1]);
    uint32_t cvtag = be32(init + 12 + 4);          /* INIT chunk: initiate tag */
    uint32_t ctsn  = be32(init + 12 + 4 + 8 + 4);   /* init TSN */
    memset(&assoc, 0, sizeof(assoc));
    assoc.dst_ip = *(uint32_t *)(pk[0] + 16); assoc.dst_port = 5001; assoc.src_port = cport;
    assoc.local_vtag = cvtag; assoc.local_tsn = ctsn; assoc.state = SCTP_STATE_COOKIE_WAIT; assoc_active = 1;

    /* 1. checksum: Linux puts the CRC32c on the wire little-endian. Every real packet of the capture must
     *    carry crc32c(packet with a zero checksum field) in that byte order, and so must our own INIT. */
    int crc_ok = 1;
    for (int i = 0; i < 11; i++) {
        uint8_t tmp[1500]; int l = pklen[i] - 20;
        memcpy(tmp, pk[i] + 20, l); memset(tmp + 8, 0, 4);
        uint32_t c = crc32c(tmp, l);
        uint8_t le[4] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24) };
        if (memcmp(pk[i] + 20 + 8, le, 4) != 0) crc_ok = 0;
    }
    check("crc32c() reproduces the checksum of all 11 real Linux packets (little-endian on the wire)", crc_ok);
    nsent = 0; send_init();
    {
        uint8_t tmp[2048]; memcpy(tmp, sent[0], sent_len[0]); memset(tmp + 8, 0, 4);
        uint32_t c = crc32c(tmp, sent_len[0]);
        uint8_t le[4] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24) };
        check("our INIT carries its CRC32c in the same (little-endian) byte order", nsent == 1 && memcmp(sent[0] + 8, le, 4) == 0);
    }

    /* 2. the real INIT-ACK is accepted and answered with a COOKIE-ECHO identical to Linux's */
    nsent = 0; feed(1);
    check("real INIT-ACK accepted (state COOKIE-ECHO)", assoc.state == SCTP_STATE_COOKIE_ECHO);
    check("260-byte cookie kept whole", assoc.cookie_len == 260);
    check("COOKIE-ECHO sent by sctp.c is byte-identical to Linux's", nsent >= 1 && sent_len[0] == pklen[2] - 20 && memcmp(sent[0], pk[2] + 20, sent_len[0]) == 0);

    /* 3. COOKIE-ACK -> established */
    feed(3);
    check("real COOKIE-ACK establishes the association", assoc.state == SCTP_STATE_ESTABLISHED);

    /* 4. our DATA equals Linux's DATA byte for byte */
    nsent = 0; sctp_send("hello-sctp", 10);
    check("DATA built by sctp.c is byte-identical to Linux's", nsent == 1 && sent_len[0] == pklen[4] - 20 && memcmp(sent[0], pk[4] + 20, sent_len[0]) == 0);

    /* 5. real DATA from the server is delivered once, in order, and SACKed */
    nsent = 0; feed(6);
    check("server DATA delivered", assoc.rx_len == 21 && memcmp(assoc.rx_buf, "world-from-linux-sctp", 21) == 0);
    check("SACK sent for it carries the right cumulative TSN", nsent >= 1 && be32(sent[0] + 12 + 4) == be32(pk[6] + 20 + 12 + 4));
    nsent = 0; feed(6);
    check("the same DATA again (duplicate) is not delivered twice", assoc.rx_len == 21);

    /* 6. a forged packet with the wrong verification tag is ignored */
    uint8_t t[1500]; memcpy(t, pk[10], pklen[10]);              /* SHUTDOWN-COMPLETE */
    ((sctp_hdr_t *)(t + 20))->vtag ^= 0x01000000u;
    /* recompute the CRC so only the tag is wrong */
    ((sctp_hdr_t *)(t + 20))->checksum = 0;
    ((sctp_hdr_t *)(t + 20))->checksum = crc32c(t + 20, pklen[10] - 20);
    sctp_handle((ip_hdr_t *)t, t + 20, pklen[10] - 20);
    check("SHUTDOWN-COMPLETE with a wrong verification tag does not tear the association down", assoc_active == 1);
    t[20 + 12] = SCTP_CHUNK_ABORT;                                /* a forged ABORT, tag wrong, CRC right */
    ((sctp_hdr_t *)(t + 20))->checksum = 0;
    ((sctp_hdr_t *)(t + 20))->checksum = crc32c(t + 20, pklen[10] - 20);
    sctp_handle((ip_hdr_t *)t, t + 20, pklen[10] - 20);
    check("forged ABORT (wrong tag) is ignored", assoc_active == 1 && assoc.state == SCTP_STATE_ESTABLISHED);

    printf("failures: %d\n", bad);
    return bad != 0;
}
