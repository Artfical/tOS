/* Prints randomised AES-128-GCM cases as hex so t_gcm_rand.py can check them against
   a known-good implementation (python-cryptography / OpenSSL). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gcm.h"
static void phex(const uint8_t *b, int n) { for (int i=0;i<n;i++) printf("%02x", b[i]); }
int main(void) {
    uint32_t s = 12345;
    for (int c = 0; c < 60; c++) {
        uint8_t k[16], n[12], a[64], p[200], ct[200], tag[16];
        int al = c % 40, pl = (c * 7) % 200;
        for (int i=0;i<16;i++){ s = s*1103515245+12345; k[i]=(uint8_t)(s>>16); }
        for (int i=0;i<12;i++){ s = s*1103515245+12345; n[i]=(uint8_t)(s>>16); }
        for (int i=0;i<al;i++){ s = s*1103515245+12345; a[i]=(uint8_t)(s>>16); }
        for (int i=0;i<pl;i++){ s = s*1103515245+12345; p[i]=(uint8_t)(s>>16); }
        aes_gcm_encrypt(k,n,a,al,p,pl,ct,tag);
        phex(k,16); printf(" "); phex(n,12); printf(" ");
        if (al) phex(a,al); else printf("-");
        printf(" ");
        if (pl) phex(p,pl); else printf("-");
        printf(" ");
        if (pl) phex(ct,pl); else printf("-");
        printf(" "); phex(tag,16); printf("\n");
    }
    return 0;
}
