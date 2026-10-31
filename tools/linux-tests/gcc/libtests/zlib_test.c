#include "common.h"
#include <zlib.h>
int main(void)
{
    CHECK(crc32(0, (const Bytef *)"123456789", 9) == 0xcbf43926u, "crc32 of 123456789");
    CHECK(adler32(1, (const Bytef *)"Wikipedia", 9) == 0x11E60398u, "adler32 of Wikipedia");
    size_t n = 5u << 20;
    unsigned char *data = make_data(n, 1);
    uLong bound = compressBound((uLong)n);
    unsigned char *z = malloc(bound), *back = malloc(n);
    for (int level = 1; level <= 9; level += 4) {
        uLongf zl = bound;
        CHECK(compress2(z, &zl, data, (uLong)n, level) == Z_OK, "compress2");
        uLongf bl = n;
        CHECK(uncompress(back, &bl, z, zl) == Z_OK && bl == n && !memcmp(back, data, n), "level round trip");
        printf("  level %d: %zu -> %lu bytes\n", level, n, (unsigned long)zl);
        CHECK(zl < n / 3, "it really compresses");
    }
    /* a gzip stream, fed in small pieces both ways */
    z_stream d; memset(&d, 0, sizeof(d));
    CHECK(deflateInit2(&d, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK, "deflateInit2 gzip");
    unsigned char *gz = malloc(bound + 64);
    d.next_out = gz; d.avail_out = (uInt)(bound + 64);
    for (size_t off = 0; off < n; off += 10007) {
        size_t k = n - off < 10007 ? n - off : 10007;
        d.next_in = data + off; d.avail_in = (uInt)k;
        int r = deflate(&d, off + k >= n ? Z_FINISH : Z_NO_FLUSH);
        CHECK(r == Z_OK || r == Z_STREAM_END, "deflate step");
    }
    size_t gzlen = d.total_out;
    deflateEnd(&d);
    CHECK(gz[0] == 0x1f && gz[1] == 0x8b, "gzip magic");
    z_stream s; memset(&s, 0, sizeof(s));
    CHECK(inflateInit2(&s, 47) == Z_OK, "inflateInit2 auto");
    size_t got = 0; int r = Z_OK;
    for (size_t off = 0; off < gzlen && r == Z_OK; off += 4093) {
        size_t k = gzlen - off < 4093 ? gzlen - off : 4093;
        s.next_in = gz + off; s.avail_in = (uInt)k;
        while (s.avail_in && r == Z_OK) {
            s.next_out = back + got; s.avail_out = (uInt)(n - got);
            r = inflate(&s, Z_NO_FLUSH);
            got = s.total_out;
        }
    }
    CHECK(r == Z_STREAM_END && got == n && !memcmp(back, data, n), "gzip stream round trip");
    inflateEnd(&s);
    /* a damaged stream is noticed */
    gz[gzlen / 2] ^= 0x55;
    memset(&s, 0, sizeof(s)); inflateInit2(&s, 47);
    s.next_in = gz; s.avail_in = (uInt)gzlen; s.next_out = back; s.avail_out = (uInt)n;
    r = inflate(&s, Z_FINISH);
    CHECK(r != Z_STREAM_END || memcmp(back, data, n), "corruption is detected");
    inflateEnd(&s);
    printf("zlib %s\n", zlibVersion());
    DONE("zlib");
}
