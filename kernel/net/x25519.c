#include "x25519.h"
#include "string.h"

/*
 * Curve25519 scalar multiplication, written straight from RFC 7748.
 *
 * Field elements are 16 limbs of 16 bits each (radix 2^16) held in int64_t, so a
 * schoolbook product of two limbs cannot overflow and carries can be deferred.
 * The prime is 2^255 - 19, which makes 2^256 congruent to 38 and turns the
 * reduction of a 32-limb product into "fold the top half back in, times 38".
 *
 * Everything here runs in time independent of the secret scalar: the ladder does the
 * same work each bit and chooses between its two states with an arithmetic mask
 * rather than a branch.
 */

typedef int64_t fe[16];

static void fe_set(fe o, const fe a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }
static void fe_zero(fe o)            { for (int i = 0; i < 16; i++) o[i] = 0; }
static void fe_one(fe o)             { fe_zero(o); o[0] = 1; }
static void fe_add(fe o, const fe a, const fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(fe o, const fe a, const fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

/* Brings every limb back into [0, 2^16); the overflow out of the top limb wraps round
 * multiplied by 38. One pass can leave limb 0 slightly over, so callers run two. */
static void fe_carry(fe o)
{
    for (int i = 0; i < 16; i++) {
        int64_t c = o[i] >> 16;              /* arithmetic shift: floor, so negatives borrow correctly */
        o[i] -= c * 65536;                   /* not c << 16: shifting a negative value is undefined */
        if (i < 15) o[i + 1] += c;
        else o[0] += 38 * c;
    }
}

static void fe_mul(fe o, const fe a, const fe b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* Exchanges a and b when `swap` is 1, touching the same memory either way. */
static void fe_cswap(fe a, fe b, int swap)
{
    int64_t mask = -(int64_t)swap;
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* o = a^(p-2) = a^-1, by the usual square-and-multiply chain over the bits of p-2
 * (every bit is 1 except positions 2 and 4). */
static void fe_inv(fe o, const fe a)
{
    fe c;
    fe_set(c, a);
    for (int i = 253; i >= 0; i--) {
        fe_sq(c, c);
        if (i != 2 && i != 4) fe_mul(c, c, a);
    }
    fe_set(o, c);
}

static void fe_unpack(fe o, const uint8_t in[32])
{
    for (int i = 0; i < 16; i++) o[i] = (int64_t)in[2 * i] | ((int64_t)in[2 * i + 1] << 8);
    o[15] &= 0x7FFF;                          /* RFC 7748: the top bit of a u-coordinate is ignored */
}

/* Fully reduces modulo 2^255-19 and writes the little-endian encoding. After the carries
 * the value is below 2^256 but may still be p or more, so p is conditionally subtracted
 * twice, choosing the result without branching on it. */
static void fe_pack(uint8_t out[32], const fe n)
{
    fe t, m;
    fe_set(t, n);
    fe_carry(t); fe_carry(t); fe_carry(t);
    for (int pass = 0; pass < 2; pass++) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        int borrow = (int)((m[15] >> 16) & 1);   /* set when t < p, i.e. the subtraction must be undone */
        m[14] &= 0xFFFF;
        fe_cswap(t, m, 1 - borrow);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)(t[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xFF);
    }
}

/* The Montgomery ladder of RFC 7748 section 5, computing the u-coordinate of scalar*point. */
static void ladder(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;                              /* clamping: clear the cofactor bits ... */
    k[31] &= 127;
    k[31] |= 64;                              /* ... and fix the top bit, so the ladder runs a fixed length */

    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
    fe_unpack(x1, point);
    fe_one(x2);  fe_zero(z2);
    fe_set(x3, x1); fe_one(z3);

    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int bit = (k[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t, da, cb);
        fe_sq(x3, t);
        fe_sub(t, da, cb);
        fe_sq(t, t);
        fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        /* z2 = E * (AA + a24*E), with a24 = 121665 */
        fe_zero(t);
        t[0] = 121665;
        fe_mul(t, e, t);
        fe_add(t, t, aa);
        fe_mul(z2, e, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_pack(out, x2);
}

void x25519_base(uint8_t public_out[32], const uint8_t scalar[32])
{
    uint8_t base[32];
    memset(base, 0, 32);
    base[0] = 9;                              /* the curve's standard base point */
    ladder(public_out, scalar, base);
}

int x25519(uint8_t shared_out[32], const uint8_t scalar[32], const uint8_t peer_public[32])
{
    ladder(shared_out, scalar, peer_public);
    /* A peer that sends a point of small order forces a shared secret of zero, which it
     * then knows in advance; RFC 7748 says to reject that result. */
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared_out[i];
    if (acc == 0) { memset(shared_out, 0, 32); return -1; }
    return 0;
}
