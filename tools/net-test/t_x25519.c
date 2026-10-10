/* Host test: X25519 against the RFC 7748 test vectors, plus the iterated test. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x25519.h"
static void unhex(const char *h, uint8_t *o) { for (int i=0;h[2*i]&&h[2*i+1];i++){
    int hi=h[2*i]<='9'?h[2*i]-'0':(h[2*i]|32)-'a'+10, lo=h[2*i+1]<='9'?h[2*i+1]-'0':(h[2*i+1]|32)-'a'+10;
    o[i]=(uint8_t)((hi<<4)|lo); } }
static int cmp(const char *name, const uint8_t *got, const char *want) {
    uint8_t w[32]; unhex(want, w);
    if (memcmp(got, w, 32)) { printf("%s MISMATCH\n  got  ", name);
        for (int i=0;i<32;i++) printf("%02x", got[i]); printf("\n  want %s\n", want); return 1; }
    printf("%s ok\n", name); return 0;
}
int main(void) {
    int bad = 0;
    uint8_t s[32], p[32], o[32];
    /* RFC 7748 section 5.2 */
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", p);
    x25519(o, s, p);
    bad += cmp("vector1", o, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", s);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", p);
    x25519(o, s, p);
    bad += cmp("vector2", o, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    /* section 6.1: Alice and Bob agree on the same secret */
    uint8_t ask[32], bsk[32], apk[32], bpk[32], ss1[32], ss2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", ask);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bsk);
    x25519_base(apk, ask);
    x25519_base(bpk, bsk);
    bad += cmp("alice pub", apk, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    bad += cmp("bob pub",   bpk, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(ss1, ask, bpk);
    x25519(ss2, bsk, apk);
    bad += cmp("shared", ss1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    if (memcmp(ss1, ss2, 32)) { printf("both sides disagree\n"); bad++; }
    /* a small-order point must be refused */
    uint8_t lowz[32]; memset(lowz, 0, 32);
    if (x25519(o, ask, lowz) == 0) { printf("small-order point ACCEPTED\n"); bad++; }
    else printf("small-order rejected ok\n");
    /* section 5.2 iterated test, 1000 rounds */
    uint8_t k[32], u[32], tmp[32];
    memset(k, 0, 32); k[0] = 9; memcpy(u, k, 32);
    for (int i = 0; i < 1000; i++) { x25519(tmp, k, u); memcpy(u, k, 32); memcpy(k, tmp, 32); }
    bad += cmp("iter1000", k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
    printf(bad ? "X25519: FAILED\n" : "X25519: ok\n");
    return bad != 0;
}
