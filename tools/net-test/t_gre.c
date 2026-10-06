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
#include "gre.c"
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
    uint8_t g[64];

    gre_init();
    gre_tunnel_add(IP4(10,0,2,15), IP4(203,0,113,9), 0, 0);          /* tunnel 0: no key */
    gre_tunnel_add(IP4(10,0,2,15), IP4(198,51,100,7), 0xCAFE, 1);     /* tunnel 1: key */

    /* plain GRE (no flags) carrying 20 bytes of 'inner IP' */
    memset(g, 0, sizeof(g)); g[2] = 0x08; g[3] = 0x00;
    outer.src_ip = IP4(203,0,113,9); outer.dst_ip = IP4(10,0,2,15);
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("GRE from the configured peer", injected, 1);
    outer.src_ip = IP4(6,6,6,6);
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("GRE from an unknown host is NOT decapsulated", injected, 0);
    outer.src_ip = IP4(203,0,113,9); outer.dst_ip = IP4(10,0,2,99);
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("GRE addressed to another local address", injected, 0);
    outer.dst_ip = IP4(10,0,2,15);

    /* keyed tunnel */
    outer.src_ip = IP4(198,51,100,7);
    memset(g, 0, sizeof(g)); g[0] = 0x20; g[2] = 0x08; g[3] = 0x00; g[4] = 0; g[5] = 0; g[6] = 0xCA; g[7] = 0xFE;
    injected = 0; gre_handle(&outer, g, 8 + 20);                     bad += expect("keyed GRE with the right key", injected, 1);
    g[7] = 0xFF;
    injected = 0; gre_handle(&outer, g, 8 + 20);                     bad += expect("keyed GRE with the wrong key", injected, 0);
    memset(g, 0, sizeof(g)); g[2] = 0x08;
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("keyed tunnel, packet without key", injected, 0);
    outer.src_ip = IP4(203,0,113,9);
    memset(g, 0, sizeof(g)); g[0] = 0x20; g[2] = 0x08; g[6] = 0xCA; g[7] = 0xFE;
    injected = 0; gre_handle(&outer, g, 8 + 20);                     bad += expect("unkeyed tunnel, packet carries a key", injected, 0);

    /* version / routing bits */
    memset(g, 0, sizeof(g)); g[1] = 0x01; g[2] = 0x08;
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("GRE version 1 is refused", injected, 0);
    memset(g, 0, sizeof(g)); g[0] = 0x40; g[2] = 0x08;
    injected = 0; gre_handle(&outer, g, 4 + 20);                     bad += expect("GRE routing bit is refused", injected, 0);

    /* checksum present */
    memset(g, 0, sizeof(g)); g[0] = 0x80; g[2] = 0x08; memset(g + 8, 0x11, 20);
    uint16_t c = csum16(g, 8 + 20); g[4] = (uint8_t)(c >> 8); g[5] = (uint8_t)c;
    injected = 0; gre_handle(&outer, g, 8 + 20);                     bad += expect("GRE with a valid checksum", injected, 1);
    g[12] ^= 0x01;
    injected = 0; gre_handle(&outer, g, 8 + 20);                     bad += expect("GRE with a corrupted payload (checksum)", injected, 0);

    /* nested encapsulation must not recurse without bound */
    memset(g, 0, sizeof(g)); g[2] = 0x08;
    recurse = gre_handle; depth = max_depth = 0; injected = 0;
    gre_handle(&outer, g, 4 + 20);
    printf("nested GRE: max decapsulation depth %d\n", max_depth);
    if (max_depth > 2) { printf("FAIL unbounded GRE nesting\n"); bad++; }
    recurse = 0;

    printf("failures: %d\n", bad);
    return bad != 0;
}
