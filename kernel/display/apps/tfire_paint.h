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

static int tfp_isqrt(int v)
{
    if (v <= 0) return 0;
    int r = 0, b = 1 << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* how far (px, py) is outside a w x h box at (x, y) with corners r round (negative: inside) */
static int tfp_dist(int px, int py, int x, int y, int w, int h, int r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int qx = x + r - px > 0 ? x + r - px : px - (x + w - 1 - r) > 0 ? px - (x + w - 1 - r) : 0;
    int qy = y + r - py > 0 ? y + r - py : py - (y + h - 1 - r) > 0 ? py - (y + h - 1 - r) : 0;
    if (!qx || !qy) {
        if (qx || qy) return (qx > qy ? qx : qy) - r;
        /* inside the inner box: how far in from the nearest edge */
        int in = px - x; if (x + w - 1 - px < in) in = x + w - 1 - px;
        if (py - y < in) in = py - y;
        if (y + h - 1 - py < in) in = y + h - 1 - py;
        return -in;
    }
    return tfp_isqrt(qx * qx + qy * qy) - r;
}

/* a box-shadow: the box, its edges blurred over it->deco pixels */
static void tfp_shadow(gfx_surface_t *d, const tf_item *it, int x, int y)
{
    int b = it->deco, half = b / 2;
    int x0 = x - half - 1, y0 = y - half - 1, x1 = x + it->w + half + 1, y1 = y + it->h + half + 1;
    if (x0 < d->cx0) x0 = d->cx0;
    if (y0 < d->cy0) y0 = d->cy0;
    if (x1 > d->cx1) x1 = d->cx1;
    if (y1 > d->cy1) y1 = d->cy1;
    /* the box casting it: nothing under it (CSS clips an outer shadow to outside the box) */
    int bx = x + (int16_t)(it->text & 0xFFFF), by = y + (int16_t)(it->text >> 16);
    int bw = it->w - 2 * it->len, bh = it->h - 2 * it->len;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = d->px + py * d->w;
        for (int px = x0; px < x1; px++) {
            if (bw > 0 && bh > 0 && px >= bx && px < bx + bw && py >= by && py < by + bh &&
                (!it->radius || tfp_dist(px, py, bx, by, bw, bh, it->radius) <= 0)) continue;
            int dist = tfp_dist(px, py, x, y, it->w, it->h, it->radius);
            int a;
            if (!b) a = dist <= 0 ? 255 : 0;
            else { a = (half - dist) * 255 / b; if (a < 0) a = 0; if (a > 255) a = 255; a = a * a / 255; }   /* (a softer fall-off) */
            a = a * it->alpha / 255;
            if (a) row[px] = gfx_mix(row[px], it->color, a);
        }
    }
}

/* a linear-gradient background */
static void tfp_gradient(gfx_surface_t *d, tf_layout *L, const tf_item *it, int x, int y)
{
    tf_grad g;
    if (it->len < sizeof(g)) return;
    memcpy(&g, L->pool + it->text, sizeof(g));
    if (g.n < 2 || g.n > 4) return;
    int sn = gfx_sin(g.angle), cs = gfx_cos(g.angle);          /* x4096: the direction (sin, -cos) on the screen */
    int asn = sn < 0 ? -sn : sn, acs = cs < 0 ? -cs : cs;
    int len = (it->w * asn + it->h * acs) / 4096;               /* the gradient line, corner to corner */
    if (len < 1) len = 1;
    int cx2 = 2 * x + it->w, cy2 = 2 * y + it->h;
    int x0 = x, y0 = y, x1 = x + it->w, y1 = y + it->h;
    if (x0 < d->cx0) x0 = d->cx0;
    if (y0 < d->cy0) y0 = d->cy0;
    if (x1 > d->cx1) x1 = d->cx1;
    if (y1 > d->cy1) y1 = d->cy1;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = d->px + py * d->w;
        int Y = 2 * py + 1 - cy2;
        for (int px = x0; px < x1; px++) {
            if (it->radius && tfp_dist(px, py, x, y, it->w, it->h, it->radius) > 0) continue;
            int X = 2 * px + 1 - cx2;
            int t = (int)(((int64_t)X * sn - (int64_t)Y * cs) * 500 / ((int64_t)len * 4096)) + 500;   /* 0..1000 along it */
            uint32_t c;
            if (t <= g.pos[0]) c = g.color[0];
            else if (t >= g.pos[g.n - 1]) c = g.color[g.n - 1];
            else {
                int k = 0;
                while (k + 1 < g.n - 1 && t > g.pos[k + 1]) k++;
                int span = g.pos[k + 1] - g.pos[k], f = span > 0 ? (t - g.pos[k]) * 256 / span : 256;
                uint32_t a = g.color[k], b2 = g.color[k + 1];
                if (!(a >> 24)) a = b2 & 0xFFFFFF;
                if (!(b2 >> 24)) b2 = a & 0xFFFFFF;
                c = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int va = (int)((a >> sh) & 255), vb = (int)((b2 >> sh) & 255);
                    c |= (uint32_t)(va + (vb - va) * f / 256) << sh;
                }
            }
            int al = (int)(c >> 24) * it->alpha / 255;
            if (al >= 255) row[px] = c & 0xFFFFFF;
            else if (al) row[px] = gfx_mix(row[px], c & 0xFFFFFF, al);
        }
    }
}

/* a rectangle, a word or a picture of the page at (x, y) of s (fields are the window's own business) */
static void tfp_item(gfx_surface_t *s, tf_page *pg, tf_layout *L, const tf_item *it, int x, int y, const ui_font_t *const *fonts)
{
    if (!it->alpha) return;
    if (it->kind == IT_SHADOW) { tfp_shadow(s, it, x, y); return; }
    if (it->kind == IT_GRAD) { tfp_gradient(s, L, it, x, y); return; }
    const ui_font_t *f = fonts[it->font < tfire_font_count ? it->font : 0];
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
