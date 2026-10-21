/* Renders a page with tFire's engine and tOS's own fonts and drawing code (gfx.c), to a PNG, on the host.
 *   tools/tfire-test/build_shot.sh && /tmp/tfire_shot url|file.html out.png [width] [height] [scroll] [click:x,y ...]
 * What it draws is what tFire draws in tOS, minus the window around it. */
#define TFIRE_SHOT
#include "page_host.c"
#include "gfx.h"
#include "imgload.h"

int uihost_image_decode(const uint8_t *data, uint32_t len, int max_w, int max_h, uint8_t **rgba, int *w, int *h)
{
    uint8_t *px;
    uint32_t iw, ih;
    if (img_decode(data, len, &px, &iw, &ih, 0) != 0) return -1;
    if (iw * ih > 4000000) { free(px); return -1; }
    uint32_t tw = iw, th = ih;
    if (max_w > 0 && tw > (uint32_t)max_w) { th = th * (uint32_t)max_w / tw; tw = (uint32_t)max_w; }
    if (max_h > 0 && th > (uint32_t)max_h) { tw = tw * (uint32_t)max_h / th; th = (uint32_t)max_h; }
    if (!tw) tw = 1;
    if (!th) th = 1;
    if (tw != iw || th != ih) {
        uint8_t *sc = img_scale(px, iw, ih, tw, th);
        free(px);
        if (!sc) return -1;
        px = sc;
    }
    *rgba = px; *w = (int)tw; *h = (int)th;
    return 0;
}
/* what imgload.c may want of the desktop */
int uihost_read(const char *path, uint8_t **d, uint32_t *n) { (void)path; (void)d; (void)n; return -1; }
int uihost_write(const char *path, const void *d, uint32_t n) { (void)path; (void)d; (void)n; return -1; }

static const ui_font_t *const fonts[] = { &ui_font_small, &ui_font_regular, &ui_font_medium, &ui_font_title, &ui_font_huge };
static int r_font_of(int px, int bold) { return px <= 12 ? 0 : px <= 17 ? (bold ? 2 : 1) : px <= 36 ? 3 : 4; }
static int r_text_w(int f, const char *s, int n)
{
    char tmp[256];
    int w = 0;
    while (n > 0) { int k = n > 255 ? 255 : n; memcpy(tmp, s, (size_t)k); tmp[k] = 0; w += gfx_text_width(fonts[f], tmp); s += k; n -= k; }
    return w;
}
static int r_font_h(int f) { return fonts[f]->ascent + fonts[f]->descent; }
static int r_font_ascent(int f) { return fonts[f]->ascent; }

/* a mask (an icon: only its alpha counts) painted in colour col, scaled to w x h */
static void blit_mask(gfx_surface_t *s, const tf_picture *pic, int x, int y, int w, int h, uint32_t col)
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
            int a = row[(size_t)((int64_t)i * pic->w / w) * 4 + 3];
            if (a) dst[px] = a == 255 ? col : gfx_mix(dst[px], col, a);
        }
    }
}

/* a rectangle of the page: plain, rounded, or a rounded outline (font = 1, deco = its width) */
static void draw_rect(gfx_surface_t *s, const tf_item *it, int x, int y)
{
    if (!it->radius) { gfx_fill(s, x, y, it->w, it->h, it->color); return; }
    int r = it->radius;
    if (r > it->w / 2) r = it->w / 2;
    if (r > it->h / 2) r = it->h / 2;
    if (it->font == 1) {
        for (int k = 0; k < it->deco && k < 8; k++) gfx_rrect_outline(s, x + k, y + k, it->w - 2 * k, it->h - 2 * k, r > k ? r - k : 0, it->color, 255);
        return;
    }
    gfx_rrect(s, x, y, it->w, it->h, r, it->color, 255);
}

static void draw(tf_page *p, gfx_surface_t *s, int W, int H, int scroll)
{
    tf_layout *L = page_layout(p);
    gfx_fill(s, 0, 0, W, H, L->n ? L->page_bg : 0xFFFFFF);
    char tmp[512];
    for (int i = 0; i < L->n; i++) {
        tf_item *it = &L->items[i];
        int x = it->x, y = it->y - scroll;
        if (y > H || y + it->h < 0) continue;
        if (it->kind == IT_RECT) draw_rect(s, it, x, y);
        else if (it->kind == IT_TEXT) {
            const ui_font_t *f = fonts[it->font < 5 ? it->font : 1];
            int n = it->len < 511 ? it->len : 511;
            memcpy(tmp, L->pool + it->text, (size_t)n); tmp[n] = 0;
            if (it->deco & 4) gfx_text_italic(s, f, x, y, tmp, it->color, 255); else gfx_text(s, f, x, y, tmp, it->color, 255);
            if (it->deco & 1) gfx_hline(s, x, y + f->ascent + 2, it->w, it->color, 200);
            if (it->deco & 2) gfx_hline(s, x, y + f->ascent * 2 / 3, it->w, it->color, 220);
        } else if (it->kind == IT_IMAGE && (it->deco & 2)) {
            tf_picture *m = page_mask(p, it->node);
            if (m) blit_mask(s, m, x, y, it->w, it->h, it->color);
        } else if (it->kind == IT_IMAGE) {
            tf_picture *pic = page_picture(p, it->node);
            if (pic) { gfx_image_t im = { pic->w, pic->h, pic->rgba }; gfx_blit_scaled(s, &im, x, y, it->w, it->h, 255); }
            else gfx_fill(s, x, y, it->w, it->h, 0xEEEEF2);
        } else if (it->kind == IT_FIELD) {
            int n = it->len < 511 ? it->len : 511;
            memcpy(tmp, L->pool + it->text, (size_t)n); tmp[n] = 0;
            gfx_rrect(s, x, y, it->w, it->h, 4, it->color == 1 ? 0xE5E5EA : 0xFFFFFF, 255);
            gfx_rrect_outline(s, x, y, it->w, it->h, 4, 0x8E8E93, 255);
            gfx_text(s, fonts[it->font < 5 ? it->font : 1], x + 5, y + 3, tmp, 0x111827, 255);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s url|file.html out.png [width] [height] [scroll] [click:x,y]\n", argv[0]); return 2; }
    if (gfx_init() != 0) { fprintf(stderr, "gfx_init failed\n"); return 1; }
    tf_host host = { { r_font_of, r_text_w, r_font_h, r_font_ascent, 0 }, 0, 0 };
    int W = argc > 3 ? atoi(argv[3]) : 1200, H = argc > 4 ? atoi(argv[4]) : 900, scroll = argc > 5 ? atoi(argv[5]) : 0;
    tf_page *p;
    if (strstr(argv[1], "://")) { tf_request r; memset(&r, 0, sizeof(r)); strncpy(r.url, argv[1], sizeof(r.url) - 1); p = page_open(&r, &host); }
    else {
        FILE *fp = fopen(argv[1], "rb"); static char buf[8 << 20]; int n = (int)fread(buf, 1, sizeof(buf) - 1, fp); buf[n] = 0; fclose(fp);
        p = page_open_html("https://localhost/test.html", buf, n, &host);
    }
    uint32_t t0 = uihost_ms();
    int step = 6;
    for (;;) {
        page_tick(p, W, H);
        tf_request nav;
        if (page_take_navigation(p, &nav)) { fprintf(stderr, "  [navigate %s]\n", nav.url); page_close(p); p = page_open(&nav, &host); free(nav.body); t0 = uihost_ms(); }
        if (!page_loading(p) && uihost_ms() - t0 > 800) {
            if (step < argc && !strncmp(argv[step], "hover:", 6)) { int x, y; sscanf(argv[step] + 6, "%d,%d", &x, &y); page_hover(p, x, y + scroll); step++; t0 = uihost_ms(); continue; }
            if (step < argc && !strncmp(argv[step], "click:", 6)) { int x, y; sscanf(argv[step] + 6, "%d,%d", &x, &y); page_click(p, x, y + scroll); step++; t0 = uihost_ms(); continue; }
            break;
        }
        if (uihost_ms() - t0 > 90000) { fprintf(stderr, "  [still loading after 90 s]\n"); break; }
        usleep(2000);
    }
    { int ni = 0, ok = 0; tf_layout *L = page_layout(p); for (int i = 0; i < L->n; i++) if (L->items[i].kind == IT_IMAGE) { ni++; if (page_picture(p, L->items[i].node)) ok++; } fprintf(stderr, "  [images in layout %d, decoded %d]\n", ni, ok); }
    if (getenv("TFDUMP")) {
        /* the style of every element with a class given in TFDUMP */
        tf_doc *d = page_doc(p);
        for (tf_node *n = d->root; n; n = dom_next(n, d->root)) {
            if (n->type != TF_ELEM || !dom_attr(n, "class") || !strstr(dom_attr(n, "class"), getenv("TFDUMP"))) continue;
            tf_style *st = n->style;
            fprintf(stderr, "<%s class=%.60s> disp=%d pos=%d float=%d laid=%d %d,%d %dx%d grid_cols=[%s] areas=[%.60s] area=[%s] w=%d/%d/%d mw=%d/%d mask=[%.50s] mimg=%d bg=%08x\n",
                lwc_string_data(n->name), dom_attr(n, "class"), st ? st->display : -1, st ? st->position : -1, st ? st->floated : -1, n->laid, n->lx, n->ly, n->lw, n->lh,
                st && st->grid_cols ? st->grid_cols : "", st && st->grid_areas ? st->grid_areas : "", st && st->grid_area ? st->grid_area : "",
                st ? st->width.v : 0, st ? st->width.pct : 0, st ? st->width.autov : 0, st ? st->max_width.v : 0, st ? st->max_width.autov : 0, st && st->mask ? st->mask : "", n->mimg, st ? st->bg : 0);
        }
    }
    uint32_t *px = calloc((size_t)W * H, 4);
    gfx_surface_t s;
    gfx_surface_init(&s, px, W, H);
    draw(p, &s, W, H, scroll);
    char ppm[512];
    snprintf(ppm, sizeof(ppm), "%s.ppm", argv[2]);
    FILE *o = fopen(ppm, "wb");
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, o); }
    fclose(o);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "python3 -c \"from PIL import Image; Image.open('%s').save('%s')\" && rm -f '%s'", ppm, argv[2], ppm);
    if (system(cmd) != 0) fprintf(stderr, "PNG conversion failed\n");
    printf("%s: %d items, doc %dx%d, title [%s]\n", argv[2], page_layout(p)->n, page_layout(p)->doc_w, page_layout(p)->doc_h, page_title(p));
    page_close(p);
    return 0;
}
