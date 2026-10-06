/* Host test: GRE / IP-in-IP decapsulation must only happen for configured tunnels. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
void (*nic_send)(void *, int);
uint8_t net_mac[6];
uint32_t route_lookup(uint32_t s, uint32_t d) { (void)s; return d; }
int arp_resolve(uint32_t ip, uint8_t *mac) { (void)ip; (void)mac; return 0; }
static int injected, max_depth, depth;
void ip_handle(uint8_t *data, int len);
#include "ipip.c"
static void (*recurse)(ip_hdr_t *, void *, int);
void ip_handle(uint8_t *data, int len)
{
    (void)data; (void)len;
    injected++;
    depth++;
    if (depth > max_depth) max_depth = depth;
    if (recurse && depth < 50) {                       /* inner packet is itself an encapsulated one */
        static ip_hdr_t outer;
        outer.src_ip = IP4(203,0,113,9); outer.dst_ip = IP4(10,0,2,15);
        recurse(&outer, data, len);
    }
    depth--;
}
static uint16_t csum16(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static int expect(const char *label, int got, int want)
{
    int bad = got != want;
    printf("%s %s (injected=%d want %d)\n", bad ? "FAIL" : "PASS", label, got, want);
    return bad;
}
int main(void)
{
    int bad = 0;
    ip_hdr_t outer; memset(&outer, 0, sizeof(outer));
    uint8_t g[64]; memset(g, 0, sizeof(g));
    ipip_init();
    ipip_tunnel_add(IP4(10,0,2,15), IP4(203,0,113,9));
    outer.dst_ip = IP4(10,0,2,15);
    /* IP-in-IP */
    outer.src_ip = IP4(203,0,113,9);
    injected = 0; ipip_handle(&outer, g, 20);                         bad += expect("IPIP from the configured peer", injected, 1);
    outer.src_ip = IP4(6,6,6,6);
    injected = 0; ipip_handle(&outer, g, 20);                         bad += expect("IPIP from an unknown host is NOT decapsulated", injected, 0);
    outer.src_ip = IP4(203,0,113,9); outer.dst_ip = IP4(10,0,2,99);
    injected = 0; ipip_handle(&outer, g, 20);                         bad += expect("IPIP addressed to another local address", injected, 0);
    outer.dst_ip = IP4(10,0,2,15);
    injected = 0; ipip_handle(&outer, g, 5);                          bad += expect("IPIP shorter than an IP header", injected, 0);
    recurse = ipip_handle; depth = max_depth = 0; injected = 0;
    ipip_handle(&outer, g, 20);
    printf("nested IPIP: max decapsulation depth %d\n", max_depth);
    if (max_depth > 2) { printf("FAIL unbounded IPIP nesting\n"); bad++; }
    printf("failures: %d\n", bad);
    return bad != 0;
}
