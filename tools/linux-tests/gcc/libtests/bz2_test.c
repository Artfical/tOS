#include "common.h"
#include <bzlib.h>
int main(void)
{
    size_t n = 3u << 20;
    unsigned char *data = make_data(n, 3);
    unsigned int zl = (unsigned int)(n + n / 100 + 600);
    char *z = malloc(zl), *back = malloc(n);
    for (int bs = 1; bs <= 9; bs += 8) {
        unsigned int zlen = zl, blen = (unsigned int)n;
        CHECK(BZ2_bzBuffToBuffCompress(z, &zlen, (char *)data, (unsigned int)n, bs, 0, 30) == BZ_OK, "bzBuffToBuffCompress");
        CHECK(BZ2_bzBuffToBuffDecompress(back, &blen, z, zlen, 0, 0) == BZ_OK && blen == n && !memcmp(back, data, n), "block size round trip");
        printf("  blocksize %d: %zu -> %u bytes\n", bs, n, zlen);
        CHECK(zlen < n / 3, "it really compresses");
        if (bs == 9) {
            z[zlen / 2] ^= 0x10; blen = (unsigned int)n;
            CHECK(BZ2_bzBuffToBuffDecompress(back, &blen, z, zlen, 0, 0) != BZ_OK, "corruption is detected");
        }
    }
    /* streaming in small pieces */
    bz_stream s; memset(&s, 0, sizeof(s));
    CHECK(BZ2_bzCompressInit(&s, 5, 0, 0) == BZ_OK, "bzCompressInit");
    s.next_out = z; s.avail_out = zl;
    for (size_t off = 0; off < n; off += 9001) {
        size_t k = n - off < 9001 ? n - off : 9001;
        s.next_in = (char *)data + off; s.avail_in = (unsigned int)k;
        int r = BZ2_bzCompress(&s, off + k >= n ? BZ_FINISH : BZ_RUN);
        CHECK(r == BZ_RUN_OK || r == BZ_FINISH_OK || r == BZ_STREAM_END, "bzCompress");
    }
    unsigned int total = s.total_out_lo32;
    BZ2_bzCompressEnd(&s);
    bz_stream d; memset(&d, 0, sizeof(d));
    BZ2_bzDecompressInit(&d, 0, 0);
    d.next_in = z; d.avail_in = total; d.next_out = back; d.avail_out = (unsigned int)n;
    int r = BZ2_bzDecompress(&d);
    CHECK(r == BZ_STREAM_END && d.total_out_lo32 == n && !memcmp(back, data, n), "streaming round trip");
    BZ2_bzDecompressEnd(&d);
    printf("bzip2 %s\n", BZ2_bzlibVersion());
    DONE("bzip2");
}
