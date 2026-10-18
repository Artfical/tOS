#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../kernel/net/p256.c"
int main(void) {
    curve_init();
    uint8_t ab[32], bb[32];
    for (int i=0;i<32;i++){ ab[i]=(uint8_t)(i*7+3); bb[i]=(uint8_t)(i*13+5); }
    uint32_t a[L],b[L],am[L],bm[L],rm[L],out[L];
    bn_from_be(a,ab); bn_from_be(b,bb);
    /* make sure both are < p */
    if (bn_cmp(a,C.fp.m)>=0) bn_sub(a,C.fp.m);
    if (bn_cmp(b,C.fp.m)>=0) bn_sub(b,C.fp.m);
    uint8_t t[32];
    bn_to_be(t,a); printf("a "); for(int i=0;i<32;i++)printf("%02x",t[i]); printf("\n");
    bn_to_be(t,b); printf("b "); for(int i=0;i<32;i++)printf("%02x",t[i]); printf("\n");
    mont_enter(&C.fp,am,a); mont_enter(&C.fp,bm,b);
    mont_mul(&C.fp,rm,am,bm); mont_leave(&C.fp,out,rm);
    bn_to_be(t,out); printf("ab "); for(int i=0;i<32;i++)printf("%02x",t[i]); printf("\n");
    /* a^-1 */
    mont_pow(&C.fp,rm,am,P256_P_M2); mont_leave(&C.fp,out,rm);
    bn_to_be(t,out); printf("ainv "); for(int i=0;i<32;i++)printf("%02x",t[i]); printf("\n");
    return 0;
}
