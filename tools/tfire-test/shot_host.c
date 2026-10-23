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

#define fonts tfire_fonts
static int r_font_of(int px, int flags)
{
    int kind = (flags & 2) ? 2 : (flags & 1), best = 0, bd = 1 << 30;
    for (int i = 0; i < tfire_font_count; i++) {
        if (tfire_font_kind[i] != kind) continue;
        int d = tfire_font_size[i] - px;
        d = d < 0 ? -d * 2 + 1 : d * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int r_text_w(int f, const char *s, int n)
{
    char tmp[256];
    int w = 0;
    while (n > 0) { int k = n > 255 ? 255 : n; memcpy(tmp, s, (size_t)k); tmp[k] = 0; w += gfx_text_width(fonts[f], tmp); s += k; n -= k; }
    return w;
}
static int r_font_h(int f) { return fonts[f]->ascent + fonts[f]->descent; }
static int r_font_ascent(int f) { return fonts[f]->ascent; }

#include "apps/tfire_paint.h"

static void draw(tf_page *p, gfx_surface_t *s, int W, int H, int scroll)
{
    tf_layout *L = page_layout(p);
    gfx_fill(s, 0, 0, W, H, L->n ? L->page_bg : 0xFFFFFF);
    char tmp[512];
    for (int i = 0; i < L->n; i++) {
        tf_item *it = &L->items[i];
        int x = it->x, y = it->y - scroll;
        int reach = it->rot ? (it->w > it->h ? it->w : it->h) : it->kind == IT_SHADOW ? it->deco : 0;
        if (y - reach > H || y + it->h + reach < 0) continue;
        if (it->kind == IT_RECT || it->kind == IT_TEXT || it->kind == IT_IMAGE || it->kind == IT_SHADOW || it->kind == IT_GRAD || it->kind == IT_FRAME) tfp_item(s, p, L, it, x, y, fonts);
        else if (it->kind == IT_FIELD) {
            int n = it->len < 511 ? it->len : 511;
            memcpy(tmp, L->pool + it->text, (size_t)n); tmp[n] = 0;
            gfx_rrect(s, x, y, it->w, it->h, 4, it->color == 1 ? 0xE5E5EA : 0xFFFFFF, 255);
            gfx_rrect_outline(s, x, y, it->w, it->h, 4, 0x8E8E93, 255);
            gfx_text(s, fonts[it->font < tfire_font_count ? it->font : 0], x + 5, y + 3, tmp, 0x111827, 255);
        }
    }
}

static void save_png(tf_page *p, int W, int H, int scroll, const char *path)
{
    uint32_t *px = calloc((size_t)W * H, 4);
    gfx_surface_t s;
    gfx_surface_init(&s, px, W, H);
    draw(p, &s, W, H, scroll);
    char ppm[512];
    snprintf(ppm, sizeof(ppm), "%s.ppm", path);
    FILE *o = fopen(ppm, "wb");
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, o); }
    fclose(o);
    free(px);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "python3 -c \"from PIL import Image; Image.open('%s').save('%s')\" && rm -f '%s'", ppm, path, ppm);
    if (system(cmd) != 0) fprintf(stderr, "PNG conversion failed\n");
}

/* keeps the page going for ms (timers, what moves) */
static void run_for(tf_page *p, int W, int H, int ms)
{
    uint32_t t0 = uihost_ms();
    while ((int)(uihost_ms() - t0) < ms) { page_tick(p, W, H); usleep(2000); }
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
            /* wait:ms keeps it going; shot:file.png draws it as it is then */
            if (step < argc && !strncmp(argv[step], "wait:", 5)) { run_for(p, W, H, atoi(argv[step] + 5)); step++; continue; }
            if (step < argc && !strncmp(argv[step], "shot:", 5)) { save_png(p, W, H, scroll, argv[step] + 5); step++; continue; }
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
            if (st) fprintf(stderr, "   nxf=%d xf0=%d/%d,%d nanim=%d kf=%d dur=%d ntrans=%d origin=%d,%d\n", st->nxf, st->xf[0].kind, st->xf[0].a, st->xf[0].b, st->nanim, st->nanim ? st->anim[0].kf : -9, st->nanim ? st->anim[0].dur : 0, st->ntrans, st->origin[0].v, st->origin[1].v);
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
