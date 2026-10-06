/* Host test: IPv6 receive path (extension header chain, options, destination filtering). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "net.h"
#include "ip6.h"
#include "nic.h"
void (*nic_send)(void *, int);
int (*nic_poll)(uint8_t *, int);
uint8_t net_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
static int icmp_calls, icmp_len;
struct ip6_hdr_t_fwd;
void icmpv6_handle(ip6_hdr_t *ip6, void *pkt, int len) { (void)ip6; (void)pkt; icmp_calls++; icmp_len = len; }
int icmpv6_ndp_resolve(const uint8_t *d, uint8_t *m) { (void)d; (void)m; return -1; }
#include "ip6.c"

static uint8_t pk[1600];
static int build(const uint8_t *dst, uint8_t nh, const uint8_t *body, int blen)
{
    memset(pk, 0, sizeof(pk));
    ip6_hdr_t *h = (ip6_hdr_t *)pk;
    h->ver_tc_fl = htonl(0x60000000u);
    h->payload_len = htons((uint16_t)blen);
    h->next_header = nh; h->hop_limit = 255;
    h->src[0] = 0xFE; h->src[1] = 0x80; h->src[15] = 2;
    memcpy(h->dst, dst, 16);
    memcpy(pk + 40, body, blen);
    return 40 + blen;
}
static int bad;
static void run(const char *label, int len, int want_calls)
{
    icmp_calls = 0;
    ip6_handle(pk, len);
    int ok = icmp_calls == want_calls;
    if (!ok) bad++;
    printf("%s %s (delivered=%d want %d)\n", ok ? "PASS" : "FAIL", label, icmp_calls, want_calls);
}
int main(void)
{
    ip6_init(net_mac);
    uint8_t icmp[8] = {128, 0, 0, 0, 0, 1, 0, 1};                 /* echo request body (checksum not checked here) */
    uint8_t all_nodes[16] = {0xFF, 0x02, [15] = 1};
    uint8_t sn[16] = {0xFF, 0x02, [11] = 1, 0xFF}; memcpy(sn + 13, net_ip6 + 13, 3);
    uint8_t other[16] = {0xFE, 0x80, [15] = 9};
    uint8_t body[256]; int n;

    n = build(net_ip6, 58, icmp, 8);                               run("plain ICMPv6 to our address", n, 1);
    n = build(all_nodes, 58, icmp, 8);                             run("to all-nodes multicast", n, 1);
    n = build(sn, 58, icmp, 8);                                    run("to our solicited-node multicast", n, 1);
    n = build(other, 58, icmp, 8);                                 run("to somebody else's address", n, 0);
    { uint8_t mc[16] = {0xFF, 0x02, [15] = 0x77}; n = build(mc, 58, icmp, 8); run("to an unrelated multicast group", n, 0); }

    /* hop-by-hop with PadN, then ICMPv6 */
    memset(body, 0, sizeof(body)); body[0] = 58; body[1] = 0; body[2] = 1; body[3] = 4;   /* 8-byte header: PadN(4) */
    memcpy(body + 8, icmp, 8);
    n = build(net_ip6, 0, body, 16);                               run("hop-by-hop (PadN) before ICMPv6", n, 1);
    if (icmp_len != 8) { printf("FAIL ICMPv6 length %d, want 8\n", icmp_len); bad++; }
    /* destination options, then ICMPv6 */
    n = build(net_ip6, 60, body, 16);                              run("destination options before ICMPv6", n, 1);
    /* HBH after another header is not allowed */
    memset(body, 0, sizeof(body)); body[0] = 0; body[1] = 0; body[2] = 1; body[3] = 4;
    body[8] = 58; body[9] = 0; body[10] = 1; body[11] = 4; memcpy(body + 16, icmp, 8);
    n = build(net_ip6, 60, body, 24);                              run("hop-by-hop not first", n, 0);
    /* unknown option with action 01 (discard) / 00 (skip) */
    memset(body, 0, sizeof(body)); body[0] = 58; body[1] = 0; body[2] = 0x41; body[3] = 4; memcpy(body + 8, icmp, 8);
    n = build(net_ip6, 60, body, 16);                              run("unknown option, action 'discard'", n, 0);
    body[2] = 0x1E;
    n = build(net_ip6, 60, body, 16);                              run("unknown option, action 'skip'", n, 1);
    /* option running past its header */
    body[2] = 1; body[3] = 200;
    n = build(net_ip6, 60, body, 16);                              run("option length beyond the header", n, 0);
    /* header longer than the packet */
    memset(body, 0, sizeof(body)); body[0] = 58; body[1] = 7; memcpy(body + 8, icmp, 8);
    n = build(net_ip6, 60, body, 16);                              run("extension header longer than the payload", n, 0);
    /* routing header: segments_left 0 ignored, >0 dropped */
    memset(body, 0, sizeof(body)); body[0] = 58; body[1] = 0; body[2] = 2; body[3] = 0; memcpy(body + 8, icmp, 8);
    n = build(net_ip6, 43, body, 16);                              run("routing header, segments_left=0", n, 1);
    body[3] = 1;
    n = build(net_ip6, 43, body, 16);                              run("routing header, segments_left=1", n, 0);
    /* fragment header: no reassembly */
    memset(body, 0, sizeof(body)); body[0] = 58; body[3] = 0; memcpy(body + 8, icmp, 8);
    n = build(net_ip6, 44, body, 16);                              run("fragment header", n, 0);
    /* chain of 9 destination-option headers */
    memset(body, 0, sizeof(body));
    for (int i = 0; i < 9; i++) { body[i * 8] = (i == 8) ? 58 : 60; body[i * 8 + 1] = 0; body[i * 8 + 2] = 1; body[i * 8 + 3] = 4; }
    memcpy(body + 72, icmp, 8);
    n = build(net_ip6, 60, body, 80);                              run("chain of 10 extension headers (over the limit)", n, 0);
    for (int i = 0; i < 6; i++) { body[i * 8] = (i == 5) ? 58 : 60; }
    memcpy(body + 48, icmp, 8);
    n = build(net_ip6, 60, body, 56);                              run("chain of 6 extension headers", n, 1);
    /* jumbogram / zero payload length, truncated, no next header */
    n = build(net_ip6, 58, icmp, 8); ((ip6_hdr_t *)pk)->payload_len = 0;  run("payload length 0 (jumbo, unsupported)", n, 0);
    n = build(net_ip6, 58, icmp, 8);                               run("frame shorter than payload_len claims", n - 4, 0);
    n = build(net_ip6, 59, icmp, 8);                               run("no next header", n, 0);
    n = build(net_ip6, 6, icmp, 8);                                run("TCP over IPv6 (not implemented): ignored", n, 0);
    /* addresses */
    n = build(net_ip6, 58, icmp, 8); ((ip6_hdr_t *)pk)->src[0] = 0xFF;   run("multicast source address", n, 0);
    n = build(net_ip6, 58, icmp, 8); memcpy(((ip6_hdr_t *)pk)->src, net_ip6, 16); run("our own address as source (loop)", n, 0);
    printf("failures: %d\n", bad);
    return bad != 0;
}
