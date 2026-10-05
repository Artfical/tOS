#include "rsa.h"
#include "csprng.h"
#include "string.h"

#define MAX_LIMBS (RSA_MAX_BYTES / 4)

static void from_be(uint32_t *out, const uint8_t *in, int bytes)
{
    int limbs = bytes / 4;
    for (int i = 0; i < limbs; i++) {
        int off = bytes - 4 * (i + 1);
        out[i] = ((uint32_t)in[off] << 24) | ((uint32_t)in[off + 1] << 16) |
                 ((uint32_t)in[off + 2] << 8) | in[off + 3];
    }
}

static void to_be(uint8_t *out, const uint32_t *in, int bytes)
{
    int limbs = bytes / 4;
    for (int i = 0; i < limbs; i++) {
        int off = bytes - 4 * (i + 1);
        out[off] = (uint8_t)(in[i] >> 24);
        out[off + 1] = (uint8_t)(in[i] >> 16);
        out[off + 2] = (uint8_t)(in[i] >> 8);
        out[off + 3] = (uint8_t)in[i];
    }
}

/* a >= b ? (both len limbs) */
static int ge(const uint32_t *a, const uint32_t *b, int len)
{
    for (int i = len - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 1;
}

static void sub_in_place(uint32_t *a, const uint32_t *b, int len)
{
    uint32_t borrow = 0;
    for (int i = 0; i < len; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)t;
        borrow = (uint32_t)(t >> 63) & 1;   /* set when the 64-bit difference wrapped */
    }
}

/* r = a * b * R^-1 mod n, R = 2^(32*len); CIOS Montgomery multiplication.
 * n0inv = -n^-1 mod 2^32. a, b < n. r may alias a or b. */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b,
                     const uint32_t *n, uint32_t n0inv, int len)
{
    uint32_t t[MAX_LIMBS + 2];
    memset(t, 0, (size_t)(len + 2) * 4);
    for (int i = 0; i < len; i++) {
        uint64_t c = 0;
        for (int j = 0; j < len; j++) {
            uint64_t x = (uint64_t)a[j] * b[i] + t[j] + c;
            t[j] = (uint32_t)x;
            c = x >> 32;
        }
        uint64_t x = (uint64_t)t[len] + c;
        t[len] = (uint32_t)x;
        t[len + 1] = (uint32_t)(x >> 32);

        uint32_t m = t[0] * n0inv;
        x = (uint64_t)m * n[0] + t[0];
        c = x >> 32;
        for (int j = 1; j < len; j++) {
            x = (uint64_t)m * n[j] + t[j] + c;
            t[j - 1] = (uint32_t)x;
            c = x >> 32;
        }
        x = (uint64_t)t[len] + c;
        t[len - 1] = (uint32_t)x;
        t[len] = t[len + 1] + (uint32_t)(x >> 32);
    }
    if (t[len] || ge(t, n, len)) sub_in_place(t, n, len);
    memcpy(r, t, (size_t)len * 4);
}

int rsa_public_op(const uint8_t *mod, int mod_len, uint32_t e,
                  const uint8_t *in, int in_len, uint8_t *out)
{
    if (mod_len < RSA_MIN_BYTES || mod_len > RSA_MAX_BYTES || (mod_len & 3)) return -1;
    if (in_len != mod_len) return -1;
    if (mod[0] == 0 || !(mod[mod_len - 1] & 1)) return -1;
    if (e < 3 || !(e & 1)) return -1;

    int len = mod_len / 4;
    uint32_t n[MAX_LIMBS], a[MAX_LIMBS], r2[MAX_LIMBS], am[MAX_LIMBS], acc[MAX_LIMBS], one[MAX_LIMBS];
    from_be(n, mod, mod_len);
    from_be(a, in, in_len);
    if (ge(a, n, len)) return -1;                      /* representative out of range */

    /* n0inv = -n^-1 mod 2^32 (Newton iteration; n[0] is odd) */
    uint32_t x = n[0];
    for (int i = 0; i < 5; i++) x *= 2 - n[0] * x;
    uint32_t n0inv = (uint32_t)0 - x;

    /* r2 = R^2 mod n by 64*len modular doublings of 1 */
    memset(r2, 0, (size_t)len * 4);
    r2[0] = 1;
    for (int i = 0; i < 64 * len; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < len; j++) {
            uint32_t v = r2[j];
            r2[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry || ge(r2, n, len)) sub_in_place(r2, n, len);
    }

    mont_mul(am, a, r2, n, n0inv, len);                /* a in Montgomery form */
    memcpy(acc, am, (size_t)len * 4);
    int top = 31;
    while (!((e >> top) & 1)) top--;
    for (int bit = top - 1; bit >= 0; bit--) {
        mont_mul(acc, acc, acc, n, n0inv, len);
        if ((e >> bit) & 1) mont_mul(acc, acc, am, n, n0inv, len);
    }
    memset(one, 0, (size_t)len * 4);
    one[0] = 1;
    mont_mul(acc, acc, one, n, n0inv, len);            /* back out of Montgomery form */
    to_be(out, acc, mod_len);
    return 0;
}

/* DER DigestInfo prefixes (RFC 8017 9.2 note 1) */
static const uint8_t di_sha256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t di_sha384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const uint8_t di_sha512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_pkcs1_verify(const uint8_t *mod, int mod_len, uint32_t e, int hash,
                     const uint8_t *digest, const uint8_t *sig, int sig_len)
{
    const uint8_t *prefix;
    int plen, dlen;
    switch (hash) {
    case RSA_HASH_SHA256: prefix = di_sha256; plen = (int)sizeof(di_sha256); dlen = 32; break;
    case RSA_HASH_SHA384: prefix = di_sha384; plen = (int)sizeof(di_sha384); dlen = 48; break;
    case RSA_HASH_SHA512: prefix = di_sha512; plen = (int)sizeof(di_sha512); dlen = 64; break;
    default: return -1;
    }
    if (sig_len != mod_len) return -1;

    uint8_t em[RSA_MAX_BYTES], want[RSA_MAX_BYTES];
    if (rsa_public_op(mod, mod_len, e, sig, sig_len, em) != 0) return -1;

    int tlen = plen + dlen;
    int ps = mod_len - 3 - tlen;
    if (ps < 8) return -1;
    want[0] = 0x00;
    want[1] = 0x01;
    memset(want + 2, 0xFF, (size_t)ps);
    want[2 + ps] = 0x00;
    memcpy(want + 3 + ps, prefix, (size_t)plen);
    memcpy(want + 3 + ps + plen, digest, (size_t)dlen);

    uint8_t diff = 0;
    for (int i = 0; i < mod_len; i++) diff |= (uint8_t)(em[i] ^ want[i]);
    return diff == 0 ? 0 : -1;
}

int rsa_pkcs1_encrypt(const uint8_t *mod, int mod_len, uint32_t e,
                      const uint8_t *msg, int msg_len, uint8_t *out)
{
    if (mod_len < RSA_MIN_BYTES || mod_len > RSA_MAX_BYTES) return -1;
    int ps = mod_len - 3 - msg_len;
    if (msg_len < 0 || ps < 8) return -1;
    uint8_t em[RSA_MAX_BYTES];
    em[0] = 0x00;
    em[1] = 0x02;
    csprng_fill(em + 2, ps);
    for (int i = 0; i < ps; i++)                        /* padding string must contain no zero byte */
        while (em[2 + i] == 0) csprng_fill(&em[2 + i], 1);
    em[2 + ps] = 0x00;
    memcpy(em + 3 + ps, msg, (size_t)msg_len);
    int rc = rsa_public_op(mod, mod_len, e, em, mod_len, out);
    memset(em, 0, sizeof(em));
    return rc;
}
