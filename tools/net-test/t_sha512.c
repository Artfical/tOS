/* Host test: SHA-384/512 against openssl on assorted lengths (padding edges). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sha512.h"
int main(void) {
    int bad = 0;
    for (int len = 0; len <= 300; len++) {
        uint8_t *m = malloc(len + 1);
        for (int i = 0; i < len; i++) m[i] = (uint8_t)(i * 7 + len);
        uint8_t d5[64], d3[48];
        sha512_hash(m, len, d5);
        sha384_hash(m, len, d3);
        /* also exercise chunked update */
        sha512_t s; uint8_t c5[64];
        sha512_init(&s);
        for (int o = 0; o < len; o += 13) sha512_update(&s, m + o, (len - o) < 13 ? (len - o) : 13);
        sha512_final(&s, c5);
        if (memcmp(d5, c5, 64)) { printf("chunk mismatch %d\n", len); bad++; }
        printf("%d ", len);
        for (int i = 0; i < 64; i++) printf("%02x", d5[i]);
        printf(" ");
        for (int i = 0; i < 48; i++) printf("%02x", d3[i]);
        printf("\n");
        free(m);
    }
    return bad;
}
