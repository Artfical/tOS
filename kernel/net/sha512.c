#include "sha512.h"
#include "string.h"

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 28) ^ ROR(x, 34) ^ ROR(x, 39))
#define BSIG1(x) (ROR(x, 14) ^ ROR(x, 18) ^ ROR(x, 41))
#define SSIG0(x) (ROR(x, 1) ^ ROR(x, 8) ^ ((x) >> 7))
#define SSIG1(x) (ROR(x, 19) ^ ROR(x, 61) ^ ((x) >> 6))

static void sha512_block(sha512_t *s, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint64_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint64_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha512_init(sha512_t *s)
{
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    memcpy(s->h, iv, sizeof(iv));
    s->buf_len = 0;
    s->total_lo = 0;
    s->is384 = 0;
}

void sha384_init(sha512_t *s)
{
    static const uint64_t iv[8] = {
        0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
    };
    memcpy(s->h, iv, sizeof(iv));
    s->buf_len = 0;
    s->total_lo = 0;
    s->is384 = 1;
}

void sha512_update(sha512_t *s, const uint8_t *data, uint32_t len)
{
    s->total_lo += len;
    while (len > 0) {
        uint32_t n = 128 - s->buf_len;
        if (n > len) n = len;
        memcpy(s->buf + s->buf_len, data, n);
        s->buf_len += n;
        data += n;
        len -= n;
        if (s->buf_len == 128) { sha512_block(s, s->buf); s->buf_len = 0; }
    }
}

void sha512_final(sha512_t *s, uint8_t *out)
{
    uint64_t bits_hi = s->total_lo >> 61;      /* bits = total * 8 as a 128-bit value */
    uint64_t bits_lo = s->total_lo << 3;
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 112) {
        while (s->buf_len < 128) s->buf[s->buf_len++] = 0;
        sha512_block(s, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 112) s->buf[s->buf_len++] = 0;
    for (int i = 0; i < 8; i++) s->buf[112 + i] = (uint8_t)(bits_hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) s->buf[120 + i] = (uint8_t)(bits_lo >> (56 - 8 * i));
    sha512_block(s, s->buf);

    int words = s->is384 ? 6 : 8;
    for (int i = 0; i < words; i++)
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(s->h[i] >> (56 - 8 * j));
}

void sha512_hash(const uint8_t *data, uint32_t len, uint8_t out[64])
{
    sha512_t s;
    sha512_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}

void sha384_hash(const uint8_t *data, uint32_t len, uint8_t out[48])
{
    sha512_t s;
    sha384_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}
