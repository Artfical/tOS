/* Host test: AES-128-GCM against the NIST SP 800-38D sample vectors and, for
   randomised cases, against openssl (see t_gcm.py). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "gcm.h"

static int unhex(const char *h, uint8_t *out) {
    int n = 0;
    while (h[0] && h[1]) {
        int hi = h[0] <= '9' ? h[0]-'0' : (h[0]|32)-'a'+10;
        int lo = h[1] <= '9' ? h[1]-'0' : (h[1]|32)-'a'+10;
        out[n++] = (uint8_t)((hi<<4)|lo); h += 2;
    }
    return n;
}
static void phex(const uint8_t *b, int n) { for (int i=0;i<n;i++) printf("%02x", b[i]); }

static int check(const char *name, const char *kh, const char *nh, const char *ah,
                 const char *ph, const char *ch, const char *th) {
    uint8_t k[16], n[12], a[256], p[256], c[256], t[16], oc[256], ot[16], op[256];
    unhex(kh,k); unhex(nh,n);
    int al = unhex(ah,a), pl = unhex(ph,p);
    unhex(ch,c); unhex(th,t);
    aes_gcm_encrypt(k,n,a,al,p,pl,oc,ot);
    int bad = 0;
    if (memcmp(oc,c,pl)) { printf("%s: ciphertext mismatch\n  got ", name); phex(oc,pl); printf("\n"); bad=1; }
    if (memcmp(ot,t,16)) { printf("%s: tag mismatch\n  got ", name); phex(ot,16); printf("\n"); bad=1; }
    if (aes_gcm_decrypt(k,n,a,al,c,pl,op,t) != 0) { printf("%s: decrypt rejected a good tag\n", name); bad=1; }
    else if (memcmp(op,p,pl)) { printf("%s: decrypt plaintext mismatch\n", name); bad=1; }
    /* a flipped bit anywhere must be caught */
    uint8_t bt[16]; memcpy(bt,t,16); bt[5] ^= 0x40;
    if (aes_gcm_decrypt(k,n,a,al,c,pl,op,bt) == 0) { printf("%s: forged tag ACCEPTED\n", name); bad=1; }
    if (pl) {
        uint8_t bc[256]; memcpy(bc,c,pl); bc[pl/2] ^= 1;
        if (aes_gcm_decrypt(k,n,a,al,bc,pl,op,t) == 0) { printf("%s: modified ciphertext ACCEPTED\n", name); bad=1; }
    }
    if (!bad) printf("%s ok\n", name);
    return bad;
}

int main(void) {
    int bad = 0;
    /* NIST SP 800-38D, Appendix B, AES-128 test cases 1-4 */
    bad += check("case1", "00000000000000000000000000000000", "000000000000000000000000", "", "", "",
                 "58e2fccefa7e3061367f1d57a4e7455a");
    bad += check("case2", "00000000000000000000000000000000", "000000000000000000000000", "",
                 "00000000000000000000000000000000", "0388dace60b6a392f328c2b971b2fe78",
                 "ab6e47d42cec13bdf53a67b21257bddf");
    bad += check("case3", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "",
                 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
                 "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
                 "4d5c2af327cd64a62cf35abd2ba6fab4");
    bad += check("case4", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
                 "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                 "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
                 "5bc94fbc3221a5db94fae95ae7121a47");
    printf(bad ? "GCM: FAILED\n" : "GCM: ok\n");
    return bad != 0;
}
