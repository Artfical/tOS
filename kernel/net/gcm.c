#include "gcm.h"
#include "aes.h"
#include "string.h"

/* ---- GF(2^128) multiplication, the field GHASH is defined over ----
 *
 * The bit order is GCM's own: bit 0 of the field element is the most significant bit of
 * byte 0, so a "shift right by one" moves towards byte 15, and the reduction polynomial
 * x^128 + x^7 + x^2 + x + 1 appears as 0xE1 in the top byte. */

static void gf_mul(uint8_t z[16], const uint8_t x[16], const uint8_t y[16])
{
    uint8_t v[16], r[16];
    memcpy(v, y, 16);
    memset(r, 0, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1)
            for (int k = 0; k < 16; k++) r[k] ^= v[k];
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(z, r, 16);
}

/* GHASH over whole blocks, padding a short final block with zeros. */
static void ghash(uint8_t acc[16], const uint8_t h[16], const uint8_t *data, uint32_t len)
{
    uint8_t blk[16];
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off < 16 ? len - off : 16;
        memset(blk, 0, 16);
        memcpy(blk, data + off, n);
        for (int k = 0; k < 16; k++) acc[k] ^= blk[k];
        gf_mul(acc, acc, h);
        off += n;
    }
}

static void inc32(uint8_t ctr[16])
{
    for (int i = 15; i >= 12; i--) if (++ctr[i]) break;
}

/* CTR mode over the counter block, as GCM defines it (the counter is the last 4 bytes). */
static void gctr(const uint8_t key[16], uint8_t ctr[16], const uint8_t *in, uint32_t len, uint8_t *out)
{
    uint8_t ks[16];
    uint32_t off = 0;
    while (off < len) {
        aes128_encrypt(key, ctr, ks);
        inc32(ctr);
        uint32_t n = len - off < 16 ? len - off : 16;
        for (uint32_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
        off += n;
    }
}

/* S = GHASH(A || pad || C || pad || len(A)*8 || len(C)*8), then T = E(J0) xor S. */
static void gcm_tag(const uint8_t key[16], const uint8_t h[16], const uint8_t j0[16],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *cipher, uint32_t len, uint8_t tag[16])
{
    uint8_t s[16], lenblk[16], ek[16];
    memset(s, 0, 16);
    ghash(s, h, aad, aad_len);
    ghash(s, h, cipher, len);

    memset(lenblk, 0, 16);
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) lenblk[7 - i] = (uint8_t)(abits >> (8 * i));
    for (int i = 0; i < 8; i++) lenblk[15 - i] = (uint8_t)(cbits >> (8 * i));
    for (int k = 0; k < 16; k++) s[k] ^= lenblk[k];
    gf_mul(s, s, h);

    aes128_encrypt(key, j0, ek);
    for (int k = 0; k < 16; k++) tag[k] = (uint8_t)(ek[k] ^ s[k]);
}

/* H and the initial counter block, shared by both directions. */
static void gcm_setup(const uint8_t key[16], const uint8_t nonce[12], uint8_t h[16], uint8_t j0[16])
{
    uint8_t zero[16];
    memset(zero, 0, 16);
    aes128_encrypt(key, zero, h);
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

void aes_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *plain, uint32_t len,
                     uint8_t *cipher, uint8_t tag[16])
{
    uint8_t h[16], j0[16], ctr[16];
    gcm_setup(key, nonce, h, j0);
    memcpy(ctr, j0, 16);
    inc32(ctr);                                     /* the data starts at J0+1; J0 itself makes the tag */
    gctr(key, ctr, plain, len, cipher);
    gcm_tag(key, h, j0, aad, aad_len, cipher, len, tag);
}

int aes_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *cipher, uint32_t len,
                    uint8_t *plain, const uint8_t tag[16])
{
    uint8_t h[16], j0[16], ctr[16], want[16];
    gcm_setup(key, nonce, h, j0);
    gcm_tag(key, h, j0, aad, aad_len, cipher, len, want);

    /* The tag is checked before a single byte is decrypted, and compared in constant time:
     * handing back plaintext that failed authentication is what padding-oracle style attacks
     * feed on. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff) { memset(plain, 0, len); return -1; }

    memcpy(ctr, j0, 16);
    inc32(ctr);
    gctr(key, ctr, cipher, len, plain);
    return 0;
}
