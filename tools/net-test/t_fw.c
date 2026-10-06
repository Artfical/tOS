/* Host test for kernel/net/fw.c: rule/NAT table churn, NAT checksum + port rewriting, conntrack aging. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
uint32_t fake_now_ms;
#include "fw.c"   /* the real implementation, included so the test can look at its tables */

static uint16_t csum(const uint8_t *p, int n, uint32_t init)
{
    uint32_t s = init;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
/* full TCP/UDP checksum over pseudo header + segment (checksum field must be zero) */
static uint16_t l4_checksum(const ip_hdr_t *ip, const uint8_t *seg, int len, int proto)
{
    uint8_t ph[12];
    memcpy(ph, &ip->src_ip, 4); memcpy(ph + 4, &ip->dst_ip, 4);
    ph[8] = 0; ph[9] = (uint8_t)proto; ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    uint32_t s = 0;
    for (int i = 0; i < 12; i += 2) s += (uint32_t)((ph[i] << 8) | ph[i + 1]);
    s = (uint16_t)~csum(seg, len, 0) + s;   /* fold: ~csum gives the plain sum */
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int check_pkt(const char *label, ip_hdr_t *ip, uint8_t *seg, int len, int proto, uint16_t want_dport, uint32_t want_dst)
{
    int bad = 0;
    uint8_t copy[64]; memcpy(copy, seg, len);
    int off = proto == 6 ? 16 : 6;
    uint16_t have = (uint16_t)((copy[off] << 8) | copy[off + 1]);
    copy[off] = copy[off + 1] = 0;
    uint16_t want = l4_checksum(ip, copy, len, proto);
    if (want == 0 && proto == 17) want = 0xFFFF;
    if (have != want) { printf("  %s: L4 checksum %04x != recomputed %04x\n", label, have, want); bad++; }
    uint8_t h[20]; memcpy(h, ip, 20);
    if (csum(h, 20, 0) != 0) { printf("  %s: IP header checksum invalid\n", label); bad++; }
    if (want_dport && (uint16_t)((seg[2] << 8) | seg[3]) != want_dport) { printf("  %s: dport %u, want %u\n", label, (seg[2] << 8) | seg[3], want_dport); bad++; }
    if (want_dst && ip->dst_ip != want_dst) { printf("  %s: dst not rewritten\n", label); bad++; }
    return bad;
}

static void make_pkt(ip_hdr_t *ip, uint8_t *seg, int len, int proto, uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp)
{
    memset(ip, 0, 20); memset(seg, 0xAB, len);
    ip->ver_ihl = 0x45; ip->total_len = htons(20 + len); ip->ttl = 64; ip->protocol = (uint8_t)proto;
    ip->src_ip = src; ip->dst_ip = dst;
    seg[0] = (uint8_t)(sp >> 8); seg[1] = (uint8_t)sp; seg[2] = (uint8_t)(dp >> 8); seg[3] = (uint8_t)dp;
    int off = proto == 6 ? 16 : 6;
    seg[off] = seg[off + 1] = 0;
    if (proto == 17) { seg[4] = (uint8_t)(len >> 8); seg[5] = (uint8_t)len; }
    uint16_t c = l4_checksum(ip, seg, len, proto);
    if (proto == 17 && c == 0) c = 0xFFFF;
    seg[off] = (uint8_t)(c >> 8); seg[off + 1] = (uint8_t)c;
    uint8_t h[20]; ip->checksum = 0; memcpy(h, ip, 20);
    uint16_t hc = csum(h, 20, 0); ip->checksum = htons(hc);
}

int main(void)
{
    int bad = 0;
    fw_init();
    /* 1. table churn: far more add/delete cycles than FW_RULE_MAX / NAT_MAX */
    int f1 = 0, f2 = 0;
    for (int i = 0; i < 300; i++) {
        int r = fw_rule_add(6, 0, 0, 0, 0, 0, (uint16_t)(1000 + i), FW_DROP);
        if (r < 0) f1++; else fw_rule_del(r);
    }
    for (int i = 0; i < 100; i++) {
        int r = nat_rule_add(NAT_DNAT, IP4(10,0,2,15), 0xFFFFFFFFu, (uint16_t)(2000 + i), IP4(10,0,2,15), 9000);
        if (r < 0) f2++; else nat_rule_del(r);
    }
    printf("fw churn failures: %d, nat churn failures: %d\n", f1, f2);
    bad += f1 + f2;

    /* 1b. deleting a middle rule must keep the order of the others (first match wins) */
    fw_init();
    fw_rule_add(0, 0, 0, 0, 0, 0, 80, FW_DROP);      /* [0] drop :80 */
    fw_rule_add(0, 0, 0, 0, 0, 0, 81, FW_ACCEPT);    /* [1] */
    fw_rule_add(0, 0, 0, 0, 0, 0, 0, FW_DROP);       /* [2] drop everything else */
    fw_rule_del(1);
    ip_hdr_t ip; uint8_t seg[32];
    make_pkt(&ip, seg, 24, 6, IP4(1,2,3,4), IP4(10,0,2,15), 5555, 81);
    int v = fw_rx(&ip, seg, 24);
    printf("rule order after delete: :81 -> %s (want DROP via catch-all)\n", v == FW_DROP ? "DROP" : "ACCEPT");
    if (v != FW_DROP) bad++;

    /* 2. DNAT: address + port rewrite must leave valid IP and L4 checksums */
    fw_init();
    nat_rule_add(NAT_DNAT, IP4(10,0,2,15), 0xFFFFFFFFu, 80, IP4(10,0,2,99), 8080);
    for (int proto = 6; proto <= 17; proto += 11) {
        make_pkt(&ip, seg, 24, proto, IP4(1,2,3,4), IP4(10,0,2,15), 5555, 80);
        fw_rx(&ip, seg, 24);
        bad += check_pkt(proto == 6 ? "DNAT tcp" : "DNAT udp", &ip, seg, 24, proto, 8080, IP4(10,0,2,99));
    }
    /* 3. SNAT likewise */
    fw_init();
    nat_rule_add(NAT_SNAT, IP4(10,0,2,0), IP4(255,255,255,0), 0, IP4(192,168,1,7), 0);
    for (int proto = 6; proto <= 17; proto += 11) {
        make_pkt(&ip, seg, 24, proto, IP4(10,0,2,15), IP4(8,8,8,8), 4321, 53);
        fw_tx(&ip, seg, 24);
        bad += check_pkt(proto == 6 ? "SNAT tcp" : "SNAT udp", &ip, seg, 24, proto, 0, IP4(8,8,8,8));
        if (ip.src_ip != IP4(192,168,1,7)) { printf("  SNAT: src not rewritten\n"); bad++; }
    }

    /* 4. conntrack: entries must age out, and a full table must evict the oldest entry, not slot 0 */
    fw_init();
    fake_now_ms = 1000;
    make_pkt(&ip, seg, 24, 6, IP4(1,1,1,1), IP4(10,0,2,15), 1, 80);
    fw_rx(&ip, seg, 24);                              /* entry A at t=1s */
    fake_now_ms = 400000;                             /* ~6.5 minutes later */
    make_pkt(&ip, seg, 24, 6, IP4(2,2,2,2), IP4(10,0,2,15), 2, 80);
    fw_rx(&ip, seg, 24);                              /* entry B */
    int n = 0;
    for (int i = 0; i < CT_MAX; i++) if (ct_table[i].valid) n++;
    printf("conntrack entries after 6 minutes idle: %d (want 1: the stale one expired)\n", n);
    if (n != 1) bad++;
    /* a full table evicts the least recently used entry, not slot 0 */
    fw_init();
    fake_now_ms = 10000;
    for (int i = 0; i < CT_MAX; i++) {            /* fill: entry i seen at 10000 + i */
        fake_now_ms = 10000 + (uint32_t)i;
        make_pkt(&ip, seg, 24, 6, IP4(3,3,(uint8_t)(i >> 8),(uint8_t)i), IP4(10,0,2,15), 7, 80);
        fw_rx(&ip, seg, 24);
    }
    fake_now_ms = 20000;
    make_pkt(&ip, seg, 24, 6, IP4(3,3,0,0), IP4(10,0,2,15), 7, 80);   /* slot 0's connection is active again */
    fw_rx(&ip, seg, 24);
    fake_now_ms = 20001;
    make_pkt(&ip, seg, 24, 6, IP4(9,9,9,9), IP4(10,0,2,15), 7, 80);   /* new connection: table is full */
    fw_rx(&ip, seg, 24);
    int slot0_alive = ct_table[0].valid && ct_table[0].src_ip == IP4(3,3,0,0);
    printf("active connection in slot 0 survived eviction: %s\n", slot0_alive ? "yes" : "NO");
    if (!slot0_alive) bad++;
    printf("total failures: %d\n", bad);
    return bad != 0;
}
