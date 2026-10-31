#include "common.h"
#include <lzma.h>
int main(void)
{
    size_t n = 3u << 20;
    unsigned char *data = make_data(n, 2);
    size_t cap = lzma_stream_buffer_bound(n);
    unsigned char *z = malloc(cap), *back = malloc(n);
    for (int preset = 1; preset <= 6; preset += 5) {
        size_t pos = 0;
        lzma_ret r = lzma_easy_buffer_encode((uint32_t)preset, LZMA_CHECK_CRC64, NULL, data, n, z, &pos, cap);
        CHECK(r == LZMA_OK, "lzma_easy_buffer_encode");
        uint64_t memlimit = UINT64_MAX; size_t in_pos = 0, out_pos = 0;
        r = lzma_stream_buffer_decode(&memlimit, 0, NULL, z, &in_pos, pos, back, &out_pos, n);
        CHECK(r == LZMA_OK && out_pos == n && !memcmp(back, data, n), "xz stream round trip");
        printf("  preset %d: %zu -> %zu bytes\n", preset, n, pos);
        CHECK(pos < n / 3, "it really compresses");
        if (preset == 6) {                                  /* a flipped byte is caught by the check */
            z[pos / 2] ^= 1; in_pos = out_pos = 0; memlimit = UINT64_MAX;
            r = lzma_stream_buffer_decode(&memlimit, 0, NULL, z, &in_pos, pos, back, &out_pos, n);
            CHECK(r != LZMA_OK, "corruption is detected");
        }
    }
    /* the streaming interface with a multi-threaded-free encoder, in small pieces */
    lzma_stream s = LZMA_STREAM_INIT;
    CHECK(lzma_easy_encoder(&s, 3, LZMA_CHECK_CRC32) == LZMA_OK, "lzma_easy_encoder");
    s.next_out = z; s.avail_out = cap;
    for (size_t off = 0; off < n; off += 7001) {
        size_t k = n - off < 7001 ? n - off : 7001;
        s.next_in = data + off; s.avail_in = k;
        lzma_ret r = lzma_code(&s, off + k >= n ? LZMA_FINISH : LZMA_RUN);
        CHECK(r == LZMA_OK || r == LZMA_STREAM_END, "lzma_code");
    }
    size_t zl = s.total_out; lzma_end(&s);
    lzma_stream d = LZMA_STREAM_INIT;
    CHECK(lzma_stream_decoder(&d, UINT64_MAX, 0) == LZMA_OK, "lzma_stream_decoder");
    d.next_in = z; d.avail_in = zl; d.next_out = back; d.avail_out = n;
    CHECK(lzma_code(&d, LZMA_FINISH) == LZMA_STREAM_END && d.total_out == n && !memcmp(back, data, n), "streaming round trip");
    lzma_end(&d);
    printf("liblzma %s\n", lzma_version_string());
    DONE("liblzma");
}
