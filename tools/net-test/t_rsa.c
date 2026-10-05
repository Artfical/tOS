/* Host test driver for rsa.c: reads commands on argv.
 *   verify MODHEX EXP HASH(1|2|3) DIGESTHEX SIGHEX      -> prints "ok" / "bad"
 *   enc MODHEX EXP MSGHEX                               -> prints ciphertext hex
 *   op MODHEX EXP INHEX                                 -> prints in^e mod n hex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rsa.h"
#include "csprng.h"

unsigned char net_mac[6] = {1, 2, 3, 4, 5, 6};

static int unhex(const char *h, uint8_t *out, int max)
{
    int n = 0;
    while (h[0] && h[1] && n < max) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}
static void phex(const uint8_t *p, int n) { for (int i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }

int main(int argc, char **argv)
{
    uint8_t mod[600], a[600], b[600], c[600];
    if (argc < 4) return 2;
    int ml = unhex(argv[2], mod, sizeof(mod));
    uint32_t e = (uint32_t)strtoul(argv[3], 0, 10);
    if (!strcmp(argv[1], "verify") && argc == 7) {
        int hash = atoi(argv[4]);
        int dl = unhex(argv[5], a, sizeof(a));
        int sl = unhex(argv[6], b, sizeof(b));
        (void)dl;
        printf(rsa_pkcs1_verify(mod, ml, e, hash, a, b, sl) == 0 ? "ok\n" : "bad\n");
    } else if (!strcmp(argv[1], "enc") && argc == 5) {
        int l = unhex(argv[4], a, sizeof(a));
        if (rsa_pkcs1_encrypt(mod, ml, e, a, l, c) != 0) { printf("err\n"); return 1; }
        phex(c, ml);
    } else if (!strcmp(argv[1], "op") && argc == 5) {
        int l = unhex(argv[4], a, sizeof(a));
        if (rsa_public_op(mod, ml, e, a, l, c) != 0) { printf("err\n"); return 1; }
        phex(c, ml);
    } else return 2;
    return 0;
}
