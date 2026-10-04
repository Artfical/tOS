#include "csprng.h"
#include "chacha20.h"
#include "sha256.h"
#include "net.h"
#include "string.h"
#include "debugmon.h"
#include "io.h"

static uint8_t g_key[32];
static uint32_t g_ctr;
static int g_seeded;

static inline uint64_t rd_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1;
}

static int rdrand32(uint32_t *v)
{
    for (int i = 0; i < 10; i++) {
        uint32_t x;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok) : : "cc");
        if (ok) { *v = x; return 1; }
    }
    return 0;
}

static void wipe(void *p, int n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/* key = SHA-256(key || data || fresh timing) */
static void mix(const void *data, int len)
{
    sha256_t s;
    uint8_t out[32];
    uint64_t t = rd_tsc();
    uint32_t up = debugmon_uptime_ms();
    sha256_init(&s);
    sha256_update(&s, g_key, 32);
    if (len > 0) sha256_update(&s, (const uint8_t *)data, (uint32_t)len);
    sha256_update(&s, (const uint8_t *)&t, sizeof(t));
    sha256_update(&s, (const uint8_t *)&up, sizeof(up));
    sha256_update(&s, (const uint8_t *)&g_ctr, sizeof(g_ctr));
    sha256_final(&s, out);
    memcpy(g_key, out, 32);
    wipe(out, sizeof(out));
}

static void seed(void)
{
    sha256_t s;
    uint8_t out[32];
    sha256_init(&s);

    if (cpu_has_rdrand()) {
        for (int i = 0; i < 16; i++) {
            uint32_t v;
            if (rdrand32(&v)) sha256_update(&s, (const uint8_t *)&v, 4);
        }
    }
    /* Timing jitter: the TSC delta around a PIT counter latch/read varies
     * with bus and interrupt timing. Weak per sample, so take many. */
    for (int i = 0; i < 1024; i++) {
        uint64_t t0 = rd_tsc();
        outb(0x43, 0x00);
        uint8_t lo = inb(0x40), hi = inb(0x40);
        uint64_t t1 = rd_tsc();
        uint32_t rec[3] = { (uint32_t)(t1 - t0), (uint32_t)t1, (uint32_t)(lo | (hi << 8)) };
        sha256_update(&s, (const uint8_t *)rec, sizeof(rec));
    }
    uint32_t up = debugmon_uptime_ms();
    uint32_t tk = debugmon_get_tick_count();
    uintptr_t sp = (uintptr_t)&s;
    sha256_update(&s, (const uint8_t *)&up, 4);
    sha256_update(&s, (const uint8_t *)&tk, 4);
    sha256_update(&s, (const uint8_t *)&sp, sizeof(sp));
    sha256_update(&s, net_mac, 6);
    sha256_final(&s, out);
    memcpy(g_key, out, 32);
    wipe(out, sizeof(out));
    g_seeded = 1;
}

void csprng_add_entropy(const void *data, int len)
{
    if (!g_seeded) seed();
    mix(data, len);
}

void csprng_fill(uint8_t *buf, int len)
{
    if (len <= 0) return;
    if (!g_seeded) seed();
    mix(0, 0);                       /* fold in fresh timing before every request */
    g_ctr++;

    uint8_t k0[32], blk[64];
    static const uint8_t nonce[12] = { 0 };
    memcpy(k0, g_key, 32);
    /* Block 0 becomes the next key (fast key erasure: past outputs cannot be
     * recomputed from the state after this call); the rest is output. */
    chacha20_block(k0, 0, nonce, blk);
    memcpy(g_key, blk, 32);
    int off = 0;
    int n = len < 32 ? len : 32;
    memcpy(buf, blk + 32, (size_t)n);
    off = n;
    for (uint32_t c = 1; off < len; c++) {
        chacha20_block(k0, c, nonce, blk);
        n = len - off < 64 ? len - off : 64;
        memcpy(buf + off, blk, (size_t)n);
        off += n;
    }
    wipe(k0, sizeof(k0));
    wipe(blk, sizeof(blk));
}

uint32_t csprng_u32(void)
{
    uint32_t v;
    csprng_fill((uint8_t *)&v, 4);
    return v;
}
