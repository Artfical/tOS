#ifndef TFIRE_PAINT_H
#define TFIRE_PAINT_H

/*
 * Drawing tFire's display list: rectangles, words and pictures, see-through (alpha) and turned (rot) as the
 * page's opacity and transforms say. Shared by the browser (tfire.c) and the host's screenshot tool, so what
 * one draws is what the other does. The includer has gfx.h, engine.h and the fonts.
 */

/* where (u, v) of an item (its own pixels, unturned) is covered, and in what colour: 0..255 */
typedef struct {
    const tf_item *it;
    const uint8_t *text;       /* a word: its coverage, tw x th (its size before a transform) */
    int tw, th;
    const tf_picture *pic;     /* a picture or an icon's mask */
    int mask;
} tfp_src;

static int tfp_sample(const tfp_src *s, int u, int v, uint32_t *col)
{
    const tf_item *it = s->it;
    if (it->kind == IT_RECT) {
        int r = it->radius;
        if (r > it->w / 2) r = it->w / 2;
        if (r > it->h / 2) r = it->h / 2;
        int inside = 1;
        if (r) {
            int cx = u < r ? r : u >= it->w - r ? it->w - r - 1 : u, cy = v < r ? r : v >= it->h - r ? it->h - r - 1 : v;
            int dx = u - cx, dy = v - cy;
            inside = dx * dx + dy * dy <= r * r;
            if (inside && it->font == 1) {                    /* an outline it->deco wide */
                int k = it->deco, rr = r > k ? r - k : 0;
                int in = u >= k && v >= k && u < it->w - k && v < it->h - k;
                if (in && rr) {
                    int cx2 = u < k + rr ? k + rr : u >= it->w - k - rr ? it->w - k - rr - 1 : u;
                    int cy2 = v < k + rr ? k + rr : v >= it->h - k - rr ? it->h - k - rr - 1 : v;
                    in = (u - cx2) * (u - cx2) + (v - cy2) * (v - cy2) <= rr * rr;
                }
                inside = !in;
            }
        }
        *col = it->color;
        return inside ? 255 : 0;
    }
    if (s->text) { *col = it->color; return s->text[(v * s->th / it->h) * s->tw + u * s->tw / it->w]; }
    if (s->pic && s->pic->w && s->pic->h) {
        const uint8_t *p = s->pic->rgba + ((size_t)(v * s->pic->h / it->h) * s->pic->w + (size_t)(u * s->pic->w / it->w)) * 4;
        if (s->mask) { *col = it->color; return p[3]; }
        *col = gfx_rgb(p[0], p[1], p[2]);
        return p[3];
    }
    return 0;
}

/* an item turned it->rot about its centre, its top left (unturned) at (x, y) */
static void tfp_turned(gfx_surface_t *d, const tf_item *it, int x, int y, const tfp_src *src)
{
    int deg = (it->rot >= 0 ? it->rot + 5 : it->rot - 5) / 10;
    int c = gfx_cos(deg), sn = gfx_sin(deg);                 /* x4096 */
    int w = it->w, h = it->h;
    if (w <= 0 || h <= 0) return;
    int ac = c < 0 ? -c : c, as = sn < 0 ? -sn : sn;
    int hw = (w * ac + h * as) / 8192 + 2, hh = (w * as + h * ac) / 8192 + 2;
    int cx2 = 2 * x + w, cy2 = 2 * y + h;                    /* the centre, in half pixels */
    int x0 = cx2 / 2 - hw, x1 = cx2 / 2 + hw, y0 = cy2 / 2 - hh, y1 = cy2 / 2 + hh;
    if (x0 < d->cx0) x0 = d->cx0;
    if (y0 < d->cy0) y0 = d->cy0;
    if (x1 > d->cx1) x1 = d->cx1;
    if (y1 > d->cy1) y1 = d->cy1;
    /* four samples a pixel (at its quarters), for smooth edges; positions in quarter pixels */
    for (int py = y0; py < y1; py++) {
        uint32_t *row = d->px + py * d->w;
        for (int px = x0; px < x1; px++) {
            int sum = 0;
            uint32_t col = 0;
            for (int k = 0; k < 4; k++) {
                int X = 4 * px + 1 + 2 * (k & 1) - 2 * cx2, Y = 4 * py + 1 + (k & 2) - 2 * cy2;
                int u4 = (c * X + sn * Y) / 4096, v4 = (-sn * X + c * Y) / 4096;
                int u = (u4 + 2 * w) >> 2, v = (v4 + 2 * h) >> 2;
                if (u < 0 || v < 0 || u >= w || v >= h) continue;
                uint32_t cc;
                int a = tfp_sample(src, u, v, &cc);
                if (a) { sum += a; col = cc; }
            }
            int a = sum / 4 * it->alpha / 255;
            if (a) row[px] = a >= 255 ? col : gfx_mix(row[px], col, a);
        }
    }
}

/* a mask (an icon: only its alpha counts) painted in colour col, scaled to w x h */
static void tfp_mask(gfx_surface_t *s, const tf_picture *pic, int x, int y, int w, int h, uint32_t col, int alpha)
{
    if (w <= 0 || h <= 0 || !pic->w || !pic->h) return;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < s->cy0 || py >= s->cy1) continue;
        const uint8_t *row = pic->rgba + (size_t)((int64_t)j * pic->h / h) * pic->w * 4;
        uint32_t *dst = s->px + py * s->w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < s->cx0 || px >= s->cx1) continue;
            int a = row[(size_t)((int64_t)i * pic->w / w) * 4 + 3] * alpha / 255;
            if (a) dst[px] = a == 255 ? col : gfx_mix(dst[px], col, a);
        }
    }
}

/* a rectangle of the page: plain, rounded, or a rounded outline (font = 1, deco = its width) */
static void tfp_rect(gfx_surface_t *s, const tf_item *it, int x, int y)
{
    int al = it->alpha;
    if (!it->radius) { gfx_fill_alpha(s, x, y, it->w, it->h, it->color, al); return; }
    int r = it->radius;
    if (r > it->w / 2) r = it->w / 2;
    if (r > it->h / 2) r = it->h / 2;
    if (it->font == 1) {
        for (int k = 0; k < it->deco && k < 8; k++) gfx_rrect_outline(s, x + k, y + k, it->w - 2 * k, it->h - 2 * k, r > k ? r - k : 0, it->color, al);
        return;
    }
    gfx_rrect(s, x, y, it->w, it->h, r, it->color, al);
}

/* a rectangle, a word or a picture of the page at (x, y) of s (fields are the window's own business) */
static void tfp_item(gfx_surface_t *s, tf_page *pg, tf_layout *L, const tf_item *it, int x, int y, const ui_font_t *const *fonts)
{
    if (!it->alpha) return;
    const ui_font_t *f = fonts[it->font < 5 ? it->font : 1];
    char tmp[512];
    int turned = it->rot && !(it->kind == IT_RECT && it->radius * 2 >= it->w && it->w == it->h);   /* (a circle looks the same) */
    if (it->kind == IT_TEXT) {
        int n = it->len < 511 ? it->len : 511;
        memcpy(tmp, L->pool + it->text, (size_t)n);
        tmp[n] = 0;
        int tw = it->ow ? it->ow : it->w, th = it->oh ? it->oh : it->h;
        int scaled = (tw != it->w || th != it->h) && it->w > 0 && it->h > 0;
        if ((turned || scaled) && tw > 0 && th > 0 && tw * th <= 1024 * 160) {
            /* the word drawn white on black at its own size first: how much each pixel is covered; then that,
             * scaled and turned */
            uint32_t *buf = malloc((size_t)tw * th * 4);
            uint8_t *cov = malloc((size_t)tw * th);
            if (buf && cov) {
                memset(buf, 0, (size_t)tw * th * 4);
                gfx_surface_t t;
                gfx_surface_init(&t, buf, tw, th);
                if (it->deco & 4) gfx_text_italic(&t, f, 0, 0, tmp, 0xFFFFFF, 255); else gfx_text(&t, f, 0, 0, tmp, 0xFFFFFF, 255);
                if (it->deco & 1) gfx_hline(&t, 0, f->ascent + 2, tw, 0xFFFFFF, 200);
                for (int i = 0; i < tw * th; i++) cov[i] = (uint8_t)((buf[i] >> 16) & 255);
                tfp_src src = { it, cov, tw, th, 0, 0 };
                tfp_turned(s, it, x, y, &src);
            }
            free(buf);
            free(cov);
            return;
        }
        if (it->deco & 4) gfx_text_italic(s, f, x, y, tmp, it->color, it->alpha); else gfx_text(s, f, x, y, tmp, it->color, it->alpha);
        if (it->deco & 1) gfx_hline(s, x, y + f->ascent + 2, it->w, it->color, 200 * it->alpha / 255);
        if (it->deco & 2) gfx_hline(s, x, y + f->ascent * 2 / 3, it->w, it->color, 220 * it->alpha / 255);
        return;
    }
    if (it->kind == IT_RECT) {
        if (turned) { tfp_src src = { it, 0, 0, 0, 0, 0 }; tfp_turned(s, it, x, y, &src); return; }
        tfp_rect(s, it, x, y);
        return;
    }
    if (it->kind == IT_IMAGE) {
        tf_picture *pic = (it->deco & 2) ? page_mask(pg, it->node) : page_picture(pg, it->node);
        if (!pic) { if (!(it->deco & 2)) gfx_fill_alpha(s, x, y, it->w, it->h, 0xEEEEF2, it->alpha); return; }
        if (turned) { tfp_src src = { it, 0, 0, 0, pic, (it->deco & 2) != 0 }; tfp_turned(s, it, x, y, &src); return; }
        if (it->deco & 2) { tfp_mask(s, pic, x, y, it->w, it->h, it->color, it->alpha); return; }
        gfx_image_t im = { pic->w, pic->h, pic->rgba };
        if (pic->w == it->w && pic->h == it->h) gfx_blit(s, &im, x, y, it->alpha);
        else gfx_blit_scaled(s, &im, x, y, it->w, it->h, it->alpha);
    }
}

#endif
