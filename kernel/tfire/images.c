/*
 * The kinds of picture tOS's own decoders (PNG, JPEG: uihost_image_decode) do not know: SVG, drawn with nanosvg
 * (third_party/nanosvg, zlib licence), and WebP, decoded with Google's libwebp (third_party/libwebp, BSD licence).
 *
 * Built for software floating point like the rest of the engine: nanosvg's floats stay in here.
 */
#include "images.h"
#include "tfire_libc.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <math.h>
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "src/webp/decode.h"
#pragma GCC diagnostic pop

int tf_image_kind(const uint8_t *d, uint32_t n)
{
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return TF_IMG_WEBP;
    uint32_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;          /* (a UTF-8 byte order mark) */
    while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) i++;
    if (i < n && d[i] == '<') {
        /* XML: an SVG if an <svg element is near the start */
        uint32_t lim = n < i + 4096 ? n : i + 4096;
        for (uint32_t k = i; k + 4 < lim; k++) if (d[k] == '<' && !memcmp(d + k + 1, "svg", 3)) return TF_IMG_SVG;
    }
    return TF_IMG_OTHER;
}

/* nearest-neighbour scaling of RGBA (for pictures bigger than they will be shown) */
static uint8_t *scale_rgba(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *o = malloc((size_t)dw * dh * 4);
    if (!o) return 0;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * sw / dw);
            memcpy(o + ((size_t)y * dw + x) * 4, src + ((size_t)sy * sw + sx) * 4, 4);
        }
    }
    return o;
}

static void fit(int *w, int *h, int max_w, int max_h)
{
    if (max_w > 0 && *w > max_w) { *h = (int)((int64_t)*h * max_w / *w); *w = max_w; }
    if (max_h > 0 && *h > max_h) { *w = (int)((int64_t)*w * max_h / *h); *h = max_h; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

int tf_svg_decode(const uint8_t *d, uint32_t n, int want_w, int want_h, int max_w, int max_h, uint8_t **rgba, int *w, int *h)
{
    if (n > (4u << 20)) return -1;
    char *text = malloc(n + 1);                    /* nanosvg parses in place, 0-terminated */
    if (!text) return -1;
    memcpy(text, d, n);
    text[n] = 0;
    NSVGimage *img = nsvgParse(text, "px", 96.0f);
    free(text);
    if (!img) return -1;
    float iw = img->width, ih = img->height;
    if (iw <= 0 || ih <= 0) { nsvgDelete(img); return -1; }
    /* drawn at the size the page asks for (twice that, so it stays sharp when it is scaled), or its own */
    int tw = want_w > 0 ? want_w * 2 : (int)(iw * 2), th;
    if (want_w <= 0 && want_h > 0) tw = (int)(iw * want_h * 2 / ih);
    if (tw < 8) tw = 8;
    if (tw > 2048) tw = 2048;
    th = (int)(ih * tw / iw);
    if (th < 1) th = 1;
    fit(&tw, &th, max_w > 0 ? max_w * 2 : 0, max_h > 0 ? max_h * 2 : 0);
    if ((int64_t)tw * th > 4000000) { nsvgDelete(img); return -1; }
    uint8_t *px = calloc((size_t)tw * th, 4);
    NSVGrasterizer *r = px ? nsvgCreateRasterizer() : 0;
    if (!r) { free(px); nsvgDelete(img); return -1; }
    nsvgRasterize(r, img, 0, 0, (float)tw / iw, px, tw, th, tw * 4);
    nsvgDeleteRasterizer(r);
    nsvgDelete(img);
    *rgba = px;
    *w = tw;
    *h = th;
    return 0;
}

int tf_webp_decode(const uint8_t *d, uint32_t n, int max_w, int max_h, uint8_t **rgba, int *w, int *h)
{
    int iw, ih;
    if (!WebPGetInfo(d, n, &iw, &ih) || iw <= 0 || ih <= 0 || (int64_t)iw * ih > 4000000) return -1;
    uint8_t *px = WebPDecodeRGBA(d, n, &iw, &ih);
    if (!px) return -1;
    int tw = iw, th = ih;
    fit(&tw, &th, max_w, max_h);
    uint8_t *out;
    if (tw != iw || th != ih) out = scale_rgba(px, iw, ih, tw, th);
    else {
        out = malloc((size_t)tw * th * 4);
        if (out) memcpy(out, px, (size_t)tw * th * 4);
    }
    WebPFree(px);
    if (!out) return -1;
    *rgba = out;
    *w = tw;
    *h = th;
    return 0;
}
