#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define P256_DEBUG 1
#include "../../kernel/net/p256.c"
static void ph(const char *tag, const uint32_t *a) { uint8_t b[32]; bn_to_be(b,a); printf("%s ",tag); for(int i=0;i<32;i++)printf("%02x",b[i]); printf("\n"); }
int main(void) {
    curve_init();
    printf("n0inv %08x\n", C.fp.n0inv);
    ph("r2   ", C.fp.r2);
    uint32_t t[L], out[L];
    /* 1 entered and left should be 1 */
    bn_zero(t); t[0]=1;
    mont_enter(&C.fp,out,t); mont_leave(&C.fp,out,out); ph("one  ", out);
    /* 2*3 mod p */
    uint32_t a[L],b[L],am[L],bm[L],rm[L];
    bn_zero(a); a[0]=2; bn_zero(b); b[0]=3;
    mont_enter(&C.fp,am,a); mont_enter(&C.fp,bm,b);
    mont_mul(&C.fp,rm,am,bm); mont_leave(&C.fp,out,rm); ph("2*3  ", out);
    /* Gx, Gy round trip */
    mont_leave(&C.fp,out,C.G.X); ph("Gx   ", out);
    mont_leave(&C.fp,out,C.G.Y); ph("Gy   ", out);
    /* G on curve? */
    uint32_t gx[L],gy[L];
    bn_from_be(gx,P256_GX); bn_from_be(gy,P256_GY);
    printf("G on curve: %d\n", on_curve(gx,gy));
    /* 2G */
    jpoint_t d; jp_double(&d,&C.G);
    uint32_t x[L]; if (jp_affine_x(x,&d)==0) ph("2G.x ", x); else printf("2G infinity\n");
    /* 3G = 2G + G */
    jpoint_t th; jp_add(&th,&d,&C.G);
    if (jp_affine_x(x,&th)==0) ph("3G.x ", x); else printf("3G infinity\n");
    /* 5*G via mul2 with b=0 */
    uint8_t k[32]; memset(k,0,32); k[31]=5;
    uint8_t z[32]; memset(z,0,32);
    jpoint_t R; jp_mul2(&R,k,&C.G,z,&C.G);
    if (jp_affine_x(x,&R)==0) ph("5G.x ", x); else printf("5G infinity\n");
    return 0;
}
