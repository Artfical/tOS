/* Host test for kernel/net/ipsec.c: AH verification against packets produced by the Linux kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
static int injected;
static uint8_t last_inner[2048]; static int last_inner_len;
void ip_handle(uint8_t *data, int len) { injected++; last_inner_len = len; memcpy(last_inner, data, len < 2048 ? len : 2048); }
#include "ipsec.c"

static uint8_t pk[3][256]; static int pklen[3];
static int bad;
static int deliver(uint8_t *p, int n)
{
    injected = 0;
    ipsec_ah_handle((ip_hdr_t *)p, p + 20, n - 20);
    return injected;
}
static void expect(const char *label, int got, int want)
{
    if (got != want) bad++;
    printf("%s %s (delivered=%d want %d)\n", got == want ? "PASS" : "FAIL", label, got, want);
}
static void fresh(const uint8_t *key, int klen)
{
    memset(ipsec_sa_table, 0, sizeof(ipsec_sa_table));
    ipsec_sa_add(IP4(10,9,0,1), 0x1000, IPPROTO_AH);
    if (key) ipsec_sa_set_key(IP4(10,9,0,1), 0x1000, key, klen);
}
int main(void)
{
    FILE *f = fopen("vectors/ah_linux.hex", "r");
    char line[1024]; int n = 0;
    while (n < 3 && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strlen(line) < 20) continue;
        int l = (int)strlen(line) / 2;
        for (int i = 0; i < l; i++) { unsigned v; sscanf(line + 2 * i, "%2x", &v); pk[n][i] = (uint8_t)v; }
        pklen[n] = l - ((line[strlen(line) - 1] == '\n') ? 1 : 0) / 2;
        n++;
    }
    for (int i = 0; i < 3; i++) pklen[i] = 20 + 32 + 8 + 10;  /* IP + 32-byte AH + UDP header + 10-byte payload "hello-ah-N" */
    uint8_t key[32]; for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i + 1);
    uint8_t t[256];

    fresh(key, 32);
    expect("real Linux AH packet, seq 1", deliver(pk[0], pklen[0]), 1);
    printf("   inner protocol delivered: %u (UDP=17), payload ok: %s\n", last_inner[9], memcmp(last_inner + 20 + 8, "hello-ah-0", 10) ? "NO" : "yes");
    expect("same packet again (replay)", deliver(pk[0], pklen[0]), 0);
    expect("seq 3", deliver(pk[2], pklen[2]), 1);
    expect("seq 2 arrives late (inside the window)", deliver(pk[1], pklen[1]), 1);
    expect("seq 2 again", deliver(pk[1], pklen[1]), 0);

    fresh(key, 32);
    memcpy(t, pk[1], pklen[1]); t[pklen[1] - 1] ^= 1;
    expect("payload byte flipped", deliver(t, pklen[1]), 0);
    memcpy(t, pk[1], pklen[1]); t[20 + 14] ^= 1;
    expect("ICV byte flipped", deliver(t, pklen[1]), 0);
    memcpy(t, pk[1], pklen[1]); t[16] ^= 1;
    expect("destination address changed (immutable field)", deliver(t, pklen[1]), 0);
    memcpy(t, pk[1], pklen[1]); t[8] = 3; t[1] = 0x28; t[6] = 0x40; t[10] = 0; t[11] = 0;
    expect("TTL, TOS and DF changed in transit (mutable fields)", deliver(t, pklen[1]), 1);

    /* a forged high sequence number must not poison the replay state */
    fresh(key, 32);
    memcpy(t, pk[0], pklen[0]); t[20 + 8] = 0xFF; t[20 + 9] = 0xFF; t[20 + 10] = 0xFF; t[20 + 11] = 0xFF;
    expect("forged seq 0xFFFFFFFF with a bad ICV", deliver(t, pklen[0]), 0);
    expect("genuine seq 2 afterwards still accepted", deliver(pk[1], pklen[1]), 1);

    uint8_t wrong[32]; memcpy(wrong, key, 32); wrong[0] ^= 1;
    fresh(wrong, 32);
    expect("wrong key", deliver(pk[0], pklen[0]), 0);
    fresh(NULL, 0);
    expect("SA without a key", deliver(pk[0], pklen[0]), 0);

    memset(ipsec_sa_table, 0, sizeof(ipsec_sa_table));
    expect("unknown SPI/peer", deliver(pk[0], pklen[0]), 0);
    int created = 0; for (int i = 0; i < IPSEC_SA_MAX; i++) created += ipsec_sa_table[i].valid;
    expect("no SA was created by that packet", created, 0);

    /* too old for the 64-packet window */
    fresh(key, 32);
    ipsec_sa_table[0].seq = 200;
    expect("sequence number 1 when 200 was already seen", deliver(pk[0], pklen[0]), 0);

    /* IP options: refused */
    fresh(key, 32);
    memcpy(t, pk[0], pklen[0]); t[0] = 0x46;
    expect("outer header with options", deliver(t, pklen[0]), 0);

    ipsec_sa_remove(0x1000);
    int left = 0; for (int i = 0; i < IPSEC_SA_MAX; i++) left += ipsec_sa_table[i].valid;
    expect("ipsec_sa_remove clears the SA", left, 0);
    printf("failures: %d\n", bad);
    return bad != 0;
}
