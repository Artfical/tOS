#include "bignum.h"
#include "string.h"

/* 2048-bit number: 64 × uint32_t, little-endian (limb 0 = least significant) */
#define BN_LIMBS 64

typedef struct { uint32_t d[BN_LIMBS]; } bn_t;
typedef struct { uint32_t d[BN_LIMBS*2]; } bn2_t;   /* 4096-bit product */

static void bn_from_be(const uint8_t *src, bn_t *out)
{
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        int off = (BN_LIMBS - 1 - i) * 4;
        out->d[i] = ((uint32_t)src[off]<<24) | ((uint32_t)src[off+1]<<16) |
                    ((uint32_t)src[off+2]<<8) | src[off+3];
    }
}

static void bn_to_be(const bn_t *src, uint8_t *out)
{
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        int off = (BN_LIMBS - 1 - i) * 4;
        out[off]   = (src->d[i] >> 24) & 0xFF;
        out[off+1] = (src->d[i] >> 16) & 0xFF;
        out[off+2] = (src->d[i] >> 8) & 0xFF;
        out[off+3] = src->d[i] & 0xFF;
    }
}

static void bn_copy(bn_t *dst, const bn_t *src)
{
    memcpy(dst->d, src->d, sizeof(dst->d));
}

/* Multiply two 2048-bit numbers → 4096-bit result */
static void bn_mul(const bn_t *a, const bn_t *b, bn2_t *out)
{
    int i, j;
    memset(out->d, 0, sizeof(out->d));
    for (i = 0; i < BN_LIMBS; i++) {
        uint64_t carry = 0;
        for (j = 0; j < BN_LIMBS; j++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + out->d[i+j] + carry;
            out->d[i+j] = (uint32_t)t;
            carry = t >> 32;
        }
        out->d[i+BN_LIMBS] += (uint32_t)carry;
    }
}

/* Compare bn2_t a with bn_t b shifted left by shift_limbs limbs.
 * Returns 1 if a > shifted_b, 0 if equal, -1 if a < shifted_b */
static int bn2_cmp_shifted(const bn2_t *a, const bn_t *b, int shift)
{
    int i;
    for (i = BN_LIMBS*2 - 1; i >= 0; i--) {
        int bi = i - shift;
        uint32_t bv = (bi >= 0 && bi < BN_LIMBS) ? b->d[bi] : 0;
        if (a->d[i] > bv) return 1;
        if (a->d[i] < bv) return -1;
    }
    return 0;
}

/* Subtract bn_t b shifted left by shift_limbs from bn2_t a (in place) */
static void bn2_sub_shifted(bn2_t *a, const bn_t *b, int shift)
{
    int i;
    int64_t borrow = 0;
    for (i = 0; i < BN_LIMBS*2; i++) {
        int bi = i - shift;
        uint32_t bv = (bi >= 0 && bi < BN_LIMBS) ? b->d[bi] : 0;
        int64_t t = (int64_t)a->d[i] - bv - borrow;
        if (t < 0) { a->d[i] = (uint32_t)(t + 0x100000000LL); borrow = 1; }
        else { a->d[i] = (uint32_t)t; borrow = 0; }
    }
}

/* Reduce: a (4096-bit) mod m (2048-bit) → out (2048-bit) */
static void bn2_mod(const bn2_t *a, const bn_t *m, bn_t *out)
{
    bn2_t r;
    int i;
    memcpy(r.d, a->d, sizeof(r.d));

    /* Find highest bit of m */
    int mhigh = -1;
    for (i = BN_LIMBS - 1; i >= 0; i--) {
        if (m->d[i]) {
            int b;
            for (b = 31; b >= 0; b--)
                if (m->d[i] >> b) { mhigh = i*32 + b; break; }
            break;
        }
    }
    if (mhigh < 0) return; /* division by zero */

    /* Binary long division: from top bit of r down to mhigh */
    int rhigh = BN_LIMBS*2*32 - 1;
    while (rhigh >= 0 && !(r.d[rhigh/32] >> (rhigh%32))) rhigh--;

    /* We work at limb granularity for speed: find how many limbs to shift */
    int shift_limbs = (rhigh / 32) - (mhigh / 32);
    while (shift_limbs >= 0) {
        if (bn2_cmp_shifted(&r, m, shift_limbs) >= 0) {
            bn2_sub_shifted(&r, m, shift_limbs);
        }
        shift_limbs--;
    }
    /* Also try bit-level shifts within the top limb alignment */
    /* After limb-level reduction, do one more bit-level pass */
    {
        bn2_t mext;
        memset(mext.d, 0, sizeof(mext.d));
        for (i = 0; i < BN_LIMBS; i++) mext.d[i] = m->d[i];
        /* count bits to shift for exact alignment */
        int rbits = -1;
        for (i = BN_LIMBS*2 - 1; i >= 0; i--)
            if (r.d[i]) { int b; for(b=31;b>=0;b--) if(r.d[i]>>b){rbits=i*32+b;break;} break; }
        int mbits = mhigh;
        int bshift = rbits - mbits; /* bit shift */
        while (bshift >= 0) {
            /* check if r >= m << bshift */
            /* We do this by building the shifted value on the fly */
            /* shift_limbs = bshift/32, bit_off = bshift%32 */
            int sl = bshift / 32;
            int bo = bshift % 32;
            /* Compare r with m<<bshift */
            int cmp = 0;
            for (i = BN_LIMBS*2 - 1; i >= 0; i--) {
                uint32_t mv = 0;
                int mi = i - sl;
                if (mi >= 0 && mi < BN_LIMBS) mv = m->d[mi] << bo;
                if (bo > 0 && mi-1 >= 0 && mi-1 < BN_LIMBS) mv |= m->d[mi-1] >> (32-bo);
                if (r.d[i] > mv) { cmp = 1; break; }
                if (r.d[i] < mv) { cmp = -1; break; }
            }
            if (cmp >= 0) {
                /* subtract m<<bshift from r */
                int64_t borrow = 0;
                for (i = 0; i < BN_LIMBS*2; i++) {
                    uint32_t mv = 0;
                    int mi = i - sl;
                    if (mi >= 0 && mi < BN_LIMBS) mv = m->d[mi] << bo;
                    if (bo > 0 && mi-1 >= 0 && mi-1 < BN_LIMBS) mv |= m->d[mi-1] >> (32-bo);
                    int64_t t = (int64_t)r.d[i] - mv - borrow;
                    if (t < 0) { r.d[i] = (uint32_t)(t + 0x100000000LL); borrow = 1; }
                    else { r.d[i] = (uint32_t)t; borrow = 0; }
                }
            }
            bshift--;
        }
    }
    for (i = 0; i < BN_LIMBS; i++) out->d[i] = r.d[i];
}

/* Modular multiply: out = a*b mod m (all 2048-bit) */
static void bn_modmul(const bn_t *a, const bn_t *b, const bn_t *m, bn_t *out)
{
    bn2_t p;
    bn_mul(a, b, &p);
    bn2_mod(&p, m, out);
}

/* base^exp mod m, exp is a regular uint32_t (e.g. 65537) */
void rsa2048_pub_encrypt(const uint8_t base[256], const uint8_t mod[256],
                         uint32_t exp, uint8_t out[256])
{
    bn_t b, m, r, tmp;
    int bit;

    bn_from_be(base, &b);
    bn_from_be(mod, &m);

    /* r = 1 */
    memset(r.d, 0, sizeof(r.d));
    r.d[0] = 1;

    /* Square-and-multiply from MSB to LSB */
    int highbit = 31;
    while (highbit > 0 && !((exp >> highbit) & 1)) highbit--;

    for (bit = highbit; bit >= 0; bit--) {
        bn_modmul(&r, &r, &m, &tmp);
        if ((exp >> bit) & 1) {
            bn_modmul(&tmp, &b, &m, &r);
        } else {
            bn_copy(&r, &tmp);
        }
    }

    bn_to_be(&r, out);
}
