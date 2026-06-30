#include <stdint.h>
#include <stddef.h>
#include "png.h"

#ifdef PNG_HOST_TEST
#include <stdlib.h>
#include <string.h>
#define PNG_MALLOC malloc
#define PNG_FREE free
#define PNG_MEMCMP memcmp
#else
#include "memory.h"
#include "string.h"
#define PNG_MALLOC malloc
#define PNG_FREE free
#define PNG_MEMCMP memcmp
#endif

#define MAXBITS 15
#define MAX_RAW_SIZE (800u * 1024u)

typedef struct {
    short count[MAXBITS + 1];
    short symbol[288];
} huff_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
    uint32_t bitbuf;
    int bitcnt;
} bitreader_t;

static int br_getbit(bitreader_t *br)
{
    if (br->bitcnt == 0) {
        if (br->pos >= br->len) return -1;
        br->bitbuf = br->data[br->pos++];
        br->bitcnt = 8;
    }
    int bit = br->bitbuf & 1;
    br->bitbuf >>= 1;
    br->bitcnt--;
    return bit;
}

static int32_t br_getbits(bitreader_t *br, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        int b = br_getbit(br);
        if (b < 0) return -1;
        val |= ((uint32_t)b) << i;
    }
    return (int32_t)val;
}

static void br_align(bitreader_t *br)
{
    br->bitbuf = 0;
    br->bitcnt = 0;
}

static void build_huffman(huff_t *h, const uint8_t *lengths, int n)
{
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
    int offs[MAXBITS + 2];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS + 1; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (short)sym;
    }
}

static int decode_symbol(bitreader_t *br, huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = br_getbit(br);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inflate_block_huffman(bitreader_t *br, uint8_t *out, uint32_t out_cap, uint32_t *op_io,
                                  huff_t *lit, huff_t *dist)
{
    uint32_t op = *op_io;
    for (;;) {
        int sym = decode_symbol(br, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (op >= out_cap) return -1;
            out[op++] = (uint8_t)sym;
        } else if (sym == 256) {
            break;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int32_t extra = length_extra[sym] ? br_getbits(br, length_extra[sym]) : 0;
            if (extra < 0) return -1;
            uint32_t length = length_base[sym] + (uint32_t)extra;

            int dsym = decode_symbol(br, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int32_t dextra = dist_extra[dsym] ? br_getbits(br, dist_extra[dsym]) : 0;
            if (dextra < 0) return -1;
            uint32_t distance = dist_base[dsym] + (uint32_t)dextra;

            if (distance > op) return -1;
            if (op + length > out_cap) return -1;
            uint32_t src = op - distance;
            for (uint32_t i = 0; i < length; i++) out[op++] = out[src++];
        }
    }
    *op_io = op;
    return 0;
}

static int inflate_raw(bitreader_t *br, uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t op = 0;
    huff_t fixed_lit, fixed_dist;
    int fixed_built = 0;

    for (;;) {
        int32_t bfinal = br_getbits(br, 1);
        int32_t btype = br_getbits(br, 2);
        if (bfinal < 0 || btype < 0) return -1;

        if (btype == 0) {
            br_align(br);
            if (br->pos + 4 > br->len) return -1;
            uint32_t lenv = br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8);
            uint32_t nlen = br->data[br->pos + 2] | ((uint32_t)br->data[br->pos + 3] << 8);
            br->pos += 4;
            if ((lenv ^ 0xFFFFu) != nlen) return -1;
            if (br->pos + lenv > br->len) return -1;
            if (op + lenv > out_cap) return -1;
            memcpy(out + op, br->data + br->pos, lenv);
            op += lenv;
            br->pos += lenv;
        } else if (btype == 1) {
            if (!fixed_built) {
                uint8_t lit_lengths[288];
                for (int i = 0; i < 144; i++) lit_lengths[i] = 8;
                for (int i = 144; i < 256; i++) lit_lengths[i] = 9;
                for (int i = 256; i < 280; i++) lit_lengths[i] = 7;
                for (int i = 280; i < 288; i++) lit_lengths[i] = 8;
                build_huffman(&fixed_lit, lit_lengths, 288);
                uint8_t dist_lengths[30];
                for (int i = 0; i < 30; i++) dist_lengths[i] = 5;
                build_huffman(&fixed_dist, dist_lengths, 30);
                fixed_built = 1;
            }
            if (inflate_block_huffman(br, out, out_cap, &op, &fixed_lit, &fixed_dist) != 0) return -1;
        } else if (btype == 2) {
            int32_t hlit_r = br_getbits(br, 5);
            int32_t hdist_r = br_getbits(br, 5);
            int32_t hclen_r = br_getbits(br, 4);
            if (hlit_r < 0 || hdist_r < 0 || hclen_r < 0) return -1;
            int hlit = hlit_r + 257;
            int hdist = hdist_r + 1;
            int hclen = hclen_r + 4;

            uint8_t clen_lengths[19];
            for (int i = 0; i < 19; i++) clen_lengths[i] = 0;
            for (int i = 0; i < hclen; i++) {
                int32_t v = br_getbits(br, 3);
                if (v < 0) return -1;
                clen_lengths[clen_order[i]] = (uint8_t)v;
            }
            huff_t clen_huff;
            build_huffman(&clen_huff, clen_lengths, 19);

            int total = hlit + hdist;
            uint8_t lengths[288 + 32];
            int idx = 0;
            while (idx < total) {
                int sym = decode_symbol(br, &clen_huff);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    int32_t rep = br_getbits(br, 2);
                    if (rep < 0) return -1;
                    rep += 3;
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- > 0 && idx < total) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int32_t rep = br_getbits(br, 3);
                    if (rep < 0) return -1;
                    rep += 3;
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                } else {
                    int32_t rep = br_getbits(br, 7);
                    if (rep < 0) return -1;
                    rep += 11;
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                }
            }

            huff_t lit, dist;
            build_huffman(&lit, lengths, hlit);
            build_huffman(&dist, lengths + hlit, hdist);
            if (inflate_block_huffman(br, out, out_cap, &op, &lit, &dist) != 0) return -1;
        } else {
            return -1;
        }

        if (bfinal) break;
    }

    *out_len = op;
    return 0;
}

int inflate_raw_buffer(const uint8_t *src, uint32_t src_len,
                        uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    bitreader_t br = { src, src_len, 0, 0, 0 };
    return inflate_raw(&br, out, out_cap, out_len);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void seterr(char *err, int cap, const char *msg)
{
    if (!err || cap <= 0) return;
    int i = 0;
    while (msg[i] && i < cap - 1) { err[i] = msg[i]; i++; }
    err[i] = 0;
}

int png_decode(const uint8_t *file, uint32_t file_len, uint8_t **out_rgb,
                uint32_t *out_w, uint32_t *out_h, char *err, int err_cap)
{
    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (file_len < 8 || PNG_MEMCMP(file, png_sig, 8) != 0) {
        seterr(err, err_cap, "not a PNG file");
        return -1;
    }

    uint32_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t bitdepth = 0, colortype = 0;
    static uint8_t palette[256][3];
    int pal_count = 0;
    int have_ihdr = 0;

    uint8_t *idat = NULL;
    uint32_t idat_len = 0, idat_cap = 0;

    while (pos + 8 <= file_len) {
        uint32_t clen = be32(file + pos);
        const uint8_t *ctype = file + pos + 4;
        const uint8_t *cdata = file + pos + 8;
        if ((uint64_t)pos + 8 + (uint64_t)clen + 4 > file_len) {
            seterr(err, err_cap, "truncated PNG chunk");
            if (idat) PNG_FREE(idat);
            return -1;
        }

        if (PNG_MEMCMP(ctype, "IHDR", 4) == 0) {
            if (clen < 13) { seterr(err, err_cap, "bad IHDR"); if (idat) PNG_FREE(idat); return -1; }
            width = be32(cdata);
            height = be32(cdata + 4);
            bitdepth = cdata[8];
            colortype = cdata[9];
            uint8_t compression = cdata[10];
            uint8_t interlace = cdata[12];
            if (compression != 0 || interlace != 0) {
                seterr(err, err_cap, "unsupported PNG (interlaced or unknown compression)");
                if (idat) PNG_FREE(idat);
                return -1;
            }
            have_ihdr = 1;
        } else if (PNG_MEMCMP(ctype, "PLTE", 4) == 0) {
            pal_count = (int)(clen / 3);
            if (pal_count > 256) pal_count = 256;
            for (int i = 0; i < pal_count; i++) {
                palette[i][0] = cdata[i * 3 + 0];
                palette[i][1] = cdata[i * 3 + 1];
                palette[i][2] = cdata[i * 3 + 2];
            }
        } else if (PNG_MEMCMP(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                uint32_t newcap = idat_cap ? idat_cap * 2 : 4096;
                while (newcap < idat_len + clen) newcap *= 2;
                uint8_t *nb = (uint8_t *)PNG_MALLOC(newcap);
                if (!nb) { seterr(err, err_cap, "out of memory"); if (idat) PNG_FREE(idat); return -1; }
                if (idat) { memcpy(nb, idat, idat_len); PNG_FREE(idat); }
                idat = nb;
                idat_cap = newcap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (PNG_MEMCMP(ctype, "IEND", 4) == 0) {
            pos += 8 + clen + 4;
            break;
        }

        pos += 8 + clen + 4;
    }

    if (!have_ihdr || !idat || width == 0 || height == 0) {
        seterr(err, err_cap, "missing IHDR/IDAT");
        if (idat) PNG_FREE(idat);
        return -1;
    }
    if (bitdepth != 8) {
        seterr(err, err_cap, "unsupported bit depth (only 8-bit PNGs)");
        PNG_FREE(idat);
        return -1;
    }

    int channels;
    switch (colortype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default:
            seterr(err, err_cap, "unsupported PNG color type");
            PNG_FREE(idat);
            return -1;
    }
    if (colortype == 3 && pal_count == 0) {
        seterr(err, err_cap, "palette PNG missing PLTE");
        PNG_FREE(idat);
        return -1;
    }

    uint32_t stride = width * (uint32_t)channels;
    uint64_t raw_cap64 = ((uint64_t)stride + 1) * height;
    if (raw_cap64 > MAX_RAW_SIZE) {
        seterr(err, err_cap, "image too large");
        PNG_FREE(idat);
        return -1;
    }
    uint32_t raw_cap = (uint32_t)raw_cap64;

    if (idat_len < 2) {
        seterr(err, err_cap, "truncated zlib stream");
        PNG_FREE(idat);
        return -1;
    }
    uint8_t cmf = idat[0], flg = idat[1];
    if ((cmf & 0x0F) != 8) {
        seterr(err, err_cap, "unsupported zlib compression method");
        PNG_FREE(idat);
        return -1;
    }
    if (flg & 0x20) {
        seterr(err, err_cap, "unsupported zlib preset dictionary");
        PNG_FREE(idat);
        return -1;
    }

    uint8_t *raw = (uint8_t *)PNG_MALLOC(raw_cap);
    if (!raw) { seterr(err, err_cap, "out of memory"); PNG_FREE(idat); return -1; }

    bitreader_t br;
    br.data = idat;
    br.len = idat_len;
    br.pos = 2;
    br.bitbuf = 0;
    br.bitcnt = 0;

    uint32_t raw_len = 0;
    int rc = inflate_raw(&br, raw, raw_cap, &raw_len);
    PNG_FREE(idat);
    if (rc != 0 || raw_len != raw_cap) {
        seterr(err, err_cap, "DEFLATE decode failed (corrupt PNG?)");
        PNG_FREE(raw);
        return -1;
    }

    uint8_t *pixels = (uint8_t *)PNG_MALLOC((uint64_t)stride * height);
    if (!pixels) { seterr(err, err_cap, "out of memory"); PNG_FREE(raw); return -1; }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *rowsrc = raw + (uint64_t)y * (stride + 1);
        uint8_t ftype = rowsrc[0];
        const uint8_t *rowdata = rowsrc + 1;
        uint8_t *outrow = pixels + (uint64_t)y * stride;
        const uint8_t *prevrow = (y == 0) ? NULL : pixels + (uint64_t)(y - 1) * stride;

        for (uint32_t x = 0; x < stride; x++) {
            uint8_t raw_byte = rowdata[x];
            int a = (x >= (uint32_t)channels) ? outrow[x - channels] : 0;
            int b = prevrow ? prevrow[x] : 0;
            int c = (prevrow && x >= (uint32_t)channels) ? prevrow[x - channels] : 0;
            uint8_t val;
            switch (ftype) {
                case 0: val = raw_byte; break;
                case 1: val = (uint8_t)(raw_byte + a); break;
                case 2: val = (uint8_t)(raw_byte + b); break;
                case 3: val = (uint8_t)(raw_byte + (uint8_t)((a + b) / 2)); break;
                case 4: {
                    int p = a + b - c;
                    int pa = p > a ? p - a : a - p;
                    int pb = p > b ? p - b : b - p;
                    int pc = p > c ? p - c : c - p;
                    int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    val = (uint8_t)(raw_byte + pred);
                    break;
                }
                default:
                    seterr(err, err_cap, "unsupported PNG filter type");
                    PNG_FREE(pixels);
                    PNG_FREE(raw);
                    return -1;
            }
            outrow[x] = val;
        }
    }
    PNG_FREE(raw);

    uint8_t *rgb = (uint8_t *)PNG_MALLOC((uint64_t)width * height * 3);
    if (!rgb) { seterr(err, err_cap, "out of memory"); PNG_FREE(pixels); return -1; }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = pixels + (uint64_t)y * stride + (uint64_t)x * channels;
            uint8_t r, g, b;
            switch (colortype) {
                case 0: r = g = b = p[0]; break;
                case 2: r = p[0]; g = p[1]; b = p[2]; break;
                case 3: {
                    int idx = p[0];
                    if (idx >= pal_count) idx = 0;
                    r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2];
                    break;
                }
                case 4: r = g = b = p[0]; break;
                default: r = p[0]; g = p[1]; b = p[2]; break;
            }
            uint8_t *out = rgb + ((uint64_t)y * width + x) * 3;
            out[0] = r; out[1] = g; out[2] = b;
        }
    }
    PNG_FREE(pixels);

    *out_rgb = rgb;
    *out_w = width;
    *out_h = height;
    return 0;
}
