/* Host test for kernel/net/wgtun.c: random nonces, anti-replay, sizes, nesting, key wipe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
#include "cmos.h"
uint8_t net_mac[6] = {1, 2, 3, 4, 5, 6}; uint32_t fake_now_ms;
static uint8_t cap[128][2048]; static int cap_len[128]; static int ncap;
int udp_send(uint32_t dst, uint16_t dp, uint16_t sp, void *data, int len)
{
    (void)dst; (void)dp; (void)sp;
    if (len > 1472) return -5;                       /* a real udp_send/ip_send refuses over-MTU datagrams */
    if (ncap < 128) { memcpy(cap[ncap], data, len); cap_len[ncap] = len; ncap++; }
    return 0;
}
static int ip_calls, nest_mode, max_nest, cur_nest;
static uint8_t last_inner[2048]; static int last_inner_len;
void wgtun_rx(uint32_t src_ip, uint16_t src_port, const uint8_t *data, int len);
int wgtun_send(int idx, const void *inner_ip, int inner_len);
void ip_handle(uint8_t *d, int l)
{
    ip_calls++; last_inner_len = l; memcpy(last_inner, d, l < 2048 ? l : 2048);
    cur_nest++; if (cur_nest > max_nest) max_nest = cur_nest;
    if (nest_mode && cur_nest < 20) {                /* the inner packet is itself an encapsulated one */
        int before = ncap; wgtun_send(0, d, l < 100 ? l : 100);
        if (ncap > before) wgtun_rx(1, 2, cap[ncap - 1], cap_len[ncap - 1]);
    }
    cur_nest--;
}
static cmos_time_t fake_time = { 0, 0, 12, 5, 10, 2026 };
int cmos_get_time(cmos_time_t *t) { *t = fake_time; return 0; }
uint32_t csprng_u32(void);
#include "wgtun.c"

static int bad;
static void check(const char *l, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", l); }
static uint8_t key[32], key2[32];
static int deliver(const uint8_t *p, int n) { ip_calls = 0; wgtun_rx(0x0100000a, 51820, p, n); return ip_calls; }
int main(void)
{
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(i * 3 + 1); key2[i] = (uint8_t)(i * 5 + 2); }
    uint8_t inner[200]; for (int i = 0; i < 200; i++) inner[i] = (uint8_t)i;

    wgtun_init();
    int idx = wgtun_add(0x0200000a, 51820, 51820, key, 0xC0FFEE);
    check("tunnel added", idx == 0);
    check("a second tunnel with the same peer id is refused", wgtun_add(0x0300000a, 1, 2, key2, 0xC0FFEE) == -1);

    ncap = 0; check("send works", wgtun_send(idx, inner, 120) == 0 && ncap == 1);
    check("packet length = 4 + 24 + 8 + 120 + 16", cap_len[0] == 4 + 24 + 8 + 120 + 16);
    check("round trip: the inner packet is delivered intact", deliver(cap[0], cap_len[0]) == 1 && last_inner_len == 120 && memcmp(last_inner, inner, 120) == 0);
    check("the same packet again is a replay and is dropped", deliver(cap[0], cap_len[0]) == 0);

    /* tampering with any byte must be caught (peer id selects the key, the rest is authenticated) */
    {
        ncap = 0; wgtun_send(idx, inner, 60);
        int rejected = 1;
        for (int i = 0; i < cap_len[0]; i++) {
            uint8_t t[2048]; memcpy(t, cap[0], cap_len[0]); t[i] ^= 0x01;
            if (deliver(t, cap_len[0])) { rejected = 0; printf("   byte %d accepted after a bit flip\n", i); break; }
        }
        check("flipping any single bit of a packet gets it rejected", rejected);
        check("the untouched packet is still accepted afterwards (state not poisoned)", deliver(cap[0], cap_len[0]) == 1);
    }

    /* random, never repeating nonces */
    {
        ncap = 0; int distinct = 1;
        static uint8_t nonces[64][24];
        for (int i = 0; i < 64; i++) { wgtun_send(idx, inner, 20); memcpy(nonces[i], cap[i] + 4, 24); }
        for (int i = 0; i < 64 && distinct; i++) for (int j = 0; j < i; j++) if (!memcmp(nonces[i], nonces[j], 24)) { distinct = 0; break; }
        check("64 packets carry 64 different random nonces", distinct);
        int counterlike = 1;
        for (int i = 0; i < 8; i++) { uint8_t c0 = cap[0][4 + i]; (void)c0; }
        counterlike = !(cap[0][4 + 8] == 0 && cap[0][4 + 9] == 0 && cap[0][4 + 10] == 0 && cap[0][4 + 11] == 0 && cap[0][4 + 12] == 0);
        check("nonce is no longer a counter padded with zeros", counterlike);
    }

    /* nonce reuse after the tunnel is recreated with the same key */
    {
        ncap = 0; wgtun_send(idx, inner, 20); uint8_t first[24]; memcpy(first, cap[0] + 4, 24);
        wgtun_del(idx); idx = wgtun_add(0x0200000a, 51820, 51820, key, 0xC0FFEE);
        ncap = 0; wgtun_send(idx, inner, 20);
        check("after wg del + add (same key) the first nonce is not the old first nonce", memcmp(first, cap[0] + 4, 24) != 0);
    }

    /* reordering inside the window is fine, replays and too-old packets are not */
    {
        ncap = 0; for (int i = 0; i < 70; i++) wgtun_send(idx, inner, 20);
        check("packet 60 (newest so far) accepted", deliver(cap[60], cap_len[60]) == 1);
        check("packet 55, late but inside the window, accepted", deliver(cap[55], cap_len[55]) == 1);
        check("packet 55 again: replay", deliver(cap[55], cap_len[55]) == 0);
        check("packet 69 accepted", deliver(cap[69], cap_len[69]) == 1);
        check("packet 2 is more than 64 behind: dropped", deliver(cap[2], cap_len[2]) == 0);
    }

    /* wrong key / wrong peer id */
    {
        ncap = 0; wgtun_send(idx, inner, 30);
        uint8_t t[2048]; memcpy(t, cap[0], cap_len[0]); t[0] ^= 1;
        check("unknown peer id is ignored", deliver(t, cap_len[0]) == 0);
        wgtun_del(idx); idx = wgtun_add(0x0200000a, 51820, 51820, key2, 0xC0FFEE);       /* same id, other key */
        check("packet made with another key fails authentication", deliver(cap[0], cap_len[0]) == 0);
    }

    /* sizes */
    {
        static uint8_t big[2000]; memset(big, 0x42, sizeof(big)); ncap = 0;
        check("negative length refused", wgtun_send(idx, big, -1) != 0 && ncap == 0);
        check("oversized inner packet refused", wgtun_send(idx, big, 1500) != 0 && ncap == 0);
        check("largest inner packet (1420) goes out in a 1472-byte datagram", wgtun_send(idx, big, WGTUN_MAX_INNER) == 0 && ncap == 1 && cap_len[0] == 1472);
        check("and is delivered", deliver(cap[0], cap_len[0]) == 1 && last_inner_len == WGTUN_MAX_INNER);
        uint8_t sm[10] = {0};
        check("a datagram shorter than the minimum is ignored", deliver(sm, 10) == 0);
        uint8_t huge[1600]; memset(huge, 0, sizeof(huge));
        check("a datagram longer than the maximum is ignored", deliver(huge, 1600) == 0);
    }

    /* nesting */
    {
        nest_mode = 1; max_nest = 0; cur_nest = 0; ncap = 0;
        wgtun_send(idx, inner, 100); wgtun_rx(1, 2, cap[0], cap_len[0]);
        nest_mode = 0;
        printf("   deepest tunnel nesting reached: %d\n", max_nest);
        check("tunnel inside tunnel is cut off after two levels", max_nest <= 2);
    }

    /* the sequence base follows the clock, so a restarted peer stays ahead of what it sent before */
    {
        wgtun_del(idx); fake_time.hour = 12; idx = wgtun_add(0x0200000a, 51820, 51820, key, 0xC0FFEE);
        for (int i = 0; i < 50; i++) { ncap = 0; wgtun_send(idx, inner, 20); }
        uint64_t last = tunnels[idx].tx_seq;
        wgtun_del(idx); fake_time.minute = 5; idx = wgtun_add(0x0200000a, 51820, 51820, key, 0xC0FFEE);
        check("after a restart five minutes later the first sequence number is above the previous ones", tunnels[idx].tx_seq > last);
    }

    /* key material is wiped on delete */
    {
        uint8_t zero[32] = {0};
        memcpy(tunnels[idx].preshared_key, key, 32);
        wgtun_t *tp = &tunnels[idx];
        wgtun_del(idx);
        check("the pre-shared key is zeroed when the tunnel is deleted", memcmp(tp->preshared_key, zero, 32) == 0);
    }
    printf("failures: %d\n", bad);
    return bad != 0;
}
