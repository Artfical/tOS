/* shared by the library tests: a check that counts failures, and data that compresses (but not trivially) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int fails_, checks_;
#define CHECK(c, what) do { checks_++; if (!(c)) { fails_++; printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)
#define DONE(lib) do { printf("%s: %s (%d checks, %d failed)\n", lib, fails_ ? "FAIL" : "PASS", checks_, fails_); return fails_ ? 1 : 0; } while (0)
/* text-like pseudo random data: words from a small vocabulary */
static unsigned char *make_data(size_t n, unsigned seed)
{
    static const char *w[] = { "tOS ", "kernel ", "compiler ", "library ", "socket ", "terminal ", "package ", "filesystem ", "thread ", "signal ", "\n", "0123456789 " };
    unsigned char *b = malloc(n + 32);
    size_t i = 0;
    uint32_t x = seed * 2654435761u + 12345u;
    while (i < n) {
        x = x * 1664525u + 1013904223u;
        const char *s = w[(x >> 20) % 12];
        for (; *s && i < n; s++) b[i++] = (unsigned char)*s;
        if (((x >> 8) & 31) == 0 && i < n) b[i++] = (unsigned char)(x >> 24);   /* now and then a random byte */
    }
    return b;
}
