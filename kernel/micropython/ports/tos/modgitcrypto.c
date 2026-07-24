/* gitcrypto module for tOS MicroPython -- SHA1 + zlib bridge for the
 * "git" tpkg package. Exposes the raw building blocks (hashing, zlib
 * inflate/deflate) that the pure-Python git client needs to speak the
 * smart-HTTP protocol and read/write pack/object data; the protocol
 * logic itself lives in Python, not here.
 *
 * zlib_decompress() reuses the existing DEFLATE decoder written for
 * PNG/zip support (inflate_raw_buffer() in kernel/display/png.c) --
 * git loose objects and pack entries are zlib streams (2-byte header +
 * deflate data + 4-byte Adler-32), same wrapping PNG's IDAT uses.
 *
 * zlib_compress() has no matching encoder to reuse, so it emits a
 * valid-but-uncompressed zlib stream (stored/raw DEFLATE blocks) --
 * spec-legal, larger on the wire, but exactly as acceptable to a git
 * server as a "real" compressed one. */
#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include <string.h>
#include <stdint.h>
#include "../../display/png.h"

/* ---- SHA1 (public domain, self-contained -- no doom/doomtype deps) ---- */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    int buf_len;
} sha1_ctx_t;

static uint32_t sha1_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx_t *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;          k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;          k = 0xCA62C1D6; }
        uint32_t tmp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0; c->buf_len = 0;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, size_t len) {
    c->len += len;
    while (len > 0) {
        size_t take = 64 - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += (int)take;
        data += take; len -= take;
        if (c->buf_len == 64) { sha1_block(c, c->buf); c->buf_len = 0; }
    }
}

static void sha1_final(sha1_ctx_t *c, uint8_t out[20]) {
    uint64_t bitlen = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buf_len != 56) sha1_update(c, &zero, 1);
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) lenbytes[i] = (uint8_t)(bitlen >> (56 - 8*i));
    /* bypass the update()'s auto-padding recursion: append length directly */
    memcpy(c->buf + 56, lenbytes, 8);
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static mp_obj_t mod_sha1(mp_obj_t data_in) {
    size_t len; const char *data = mp_obj_str_get_data(data_in, &len);
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, (const uint8_t *)data, len);
    uint8_t digest[20];
    sha1_final(&c, digest);
    return mp_obj_new_bytes(digest, 20);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_sha1_obj, mod_sha1);

/* ---- zlib bridge ---- */

static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static mp_obj_t mod_zlib_decompress(mp_obj_t data_in) {
    size_t len; const char *data = mp_obj_str_get_data(data_in, &len);
    if (len < 2) mp_raise_ValueError(MP_ERROR_TEXT("zlib: truncated stream"));
    /* skip the 2-byte zlib header (CMF/FLG); ignore the trailing 4-byte
     * Adler-32 -- inflate_raw_buffer() only wants the raw deflate body */
    const uint8_t *body = (const uint8_t *)data + 2;
    uint32_t body_len = (uint32_t)(len - 2);

    uint32_t cap = body_len * 8 + 4096;
    uint8_t *out = (uint8_t *)m_malloc(cap);
    if (!out) mp_raise_OSError(12);
    uint32_t out_len = 0;
    int rc = inflate_raw_buffer(body, body_len, out, cap, &out_len);
    if (rc != 0) { m_free(out); mp_raise_ValueError(MP_ERROR_TEXT("zlib: inflate failed")); }
    mp_obj_t ret = mp_obj_new_bytes(out, out_len);
    m_free(out);
    return ret;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_zlib_decompress_obj, mod_zlib_decompress);

/* Emits a valid zlib stream using uncompressed ("stored") DEFLATE
 * blocks -- spec-legal (RFC 1951 section 3.2.4), just not shrunk. */
static mp_obj_t mod_zlib_compress(mp_obj_t data_in) {
    size_t len; const char *data = mp_obj_str_get_data(data_in, &len);
    const size_t max_block = 65535;
    size_t nblocks = len ? (len + max_block - 1) / max_block : 1;
    size_t cap = 2 + nblocks * 5 + len + 4;
    uint8_t *out = (uint8_t *)m_malloc(cap);
    if (!out) mp_raise_OSError(12);
    size_t o = 0;
    out[o++] = 0x78; out[o++] = 0x01; /* zlib header: deflate, default window, no dict */

    size_t off = 0;
    do {
        size_t chunk = len - off;
        if (chunk > max_block) chunk = max_block;
        int is_last = (off + chunk >= len);
        out[o++] = is_last ? 1 : 0; /* BFINAL | BTYPE=00 (stored) */
        out[o++] = (uint8_t)(chunk & 0xFF);
        out[o++] = (uint8_t)((chunk >> 8) & 0xFF);
        out[o++] = (uint8_t)(~chunk & 0xFF);
        out[o++] = (uint8_t)((~chunk >> 8) & 0xFF);
        memcpy(out + o, data + off, chunk);
        o += chunk;
        off += chunk;
    } while (off < len);

    uint32_t a32 = adler32((const uint8_t *)data, len);
    out[o++] = (uint8_t)(a32 >> 24); out[o++] = (uint8_t)(a32 >> 16);
    out[o++] = (uint8_t)(a32 >> 8);  out[o++] = (uint8_t)(a32);

    mp_obj_t ret = mp_obj_new_bytes(out, o);
    m_free(out);
    return ret;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_zlib_compress_obj, mod_zlib_compress);

static void gitcrypto_store(mp_obj_dict_t *g, const char *name, const void *fn) {
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g),
        MP_OBJ_NEW_QSTR(qstr_from_str(name)), MP_OBJ_FROM_PTR(fn));
}

void gitcrypto_module_init(void) {
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("gitcrypto"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);
    gitcrypto_store(g, "sha1",            &mod_sha1_obj);
    gitcrypto_store(g, "zlib_decompress", &mod_zlib_decompress_obj);
    gitcrypto_store(g, "zlib_compress",   &mod_zlib_compress_obj);
}
