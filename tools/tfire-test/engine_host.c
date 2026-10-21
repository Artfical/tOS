/* Runs tFire's engine (kernel/tfire: dom, style, layout) on the host and prints the laid-out page as text lines.
 *   tools/tfire-test/build.sh && /tmp/tfire_engine page.html [width] [-v]
 * Style sheets: the page's <style> elements, and any <link rel=stylesheet> whose file is next to the page
 * under the name the link gives (only its last path part). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "style.h"
#include "layout.h"

/* a font that is n px high and 0.55 n wide per character, like the desktop's */
static int font_of(int px, int bold) { return (px > 63 ? 63 : px) | (bold ? 64 : 0); }
static int text_w(int f, const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if ((s[i] & 0xC0) != 0x80) c++;
    return c * (f & 63) * ((f & 64) ? 60 : 55) / 100;
}
static int font_h(int f) { return (f & 63) * 5 / 4; }
static int font_ascent(int f) { return (f & 63); }
static int image_size(tf_node *img, int *w, int *h) { (void)img; *w = *h = 0; return -1; }

static char *slurp(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *b = malloc((size_t)*len + 1);
    *len = (long)fread(b, 1, (size_t)*len, fp);
    b[*len] = 0;
    fclose(fp);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s page.html [width] [-v]\n", argv[0]); return 2; }
    long len;
    char *html = slurp(argv[1], &len);
    if (!html) { perror(argv[1]); return 1; }
    int width = argc > 2 ? atoi(argv[2]) : 900;
    int verbose = argc > 3;
    tf_doc *d = dom_parse(html, (uint32_t)len, 0);
    if (!d) { fprintf(stderr, "parse failed\n"); return 1; }
    printf("title: [%s]\n", d->title);
    tf_styler *st = style_new();
    char dir[512];
    strncpy(dir, argv[1], sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *sl = strrchr(dir, '/');
    if (sl) sl[1] = 0; else strcpy(dir, "./");
    int sheets = 0;
    for (tf_node *n = d->root; n; n = dom_next(n, d->root)) {
        if (dom_is(n, "style")) {
            char *css = dom_text_content(n);
            style_add_sheet(st, css, (uint32_t)strlen(css), "file:///");
            free(css);
            sheets++;
        } else if (dom_is(n, "link")) {
            const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
            if (!rel || !href || !strstr(rel, "stylesheet")) continue;
            const char *base = strrchr(href, '/');
            base = base ? base + 1 : href;
            char path[1024];
            snprintf(path, sizeof(path), "%s%.*s", dir, (int)strcspn(base, "?#"), base);
            long cl;
            char *css = slurp(path, &cl);
            if (!css) continue;
            style_add_sheet(st, css, (uint32_t)cl, "file:///");
            free(css);
            sheets++;
        }
    }
    style_compute(st, d, width, 700);
    tf_env env = { font_of, text_w, font_h, font_ascent, image_size };
    tf_layout L;
    memset(&L, 0, sizeof(L));
    layout_run(&L, d, width, 700, &env);
    printf("sheets %d items %d doc %dx%d bg %06x\n", sheets, L.n, L.doc_w, L.doc_h, L.page_bg);
    int y = -100000;
    for (int i = 0; i < L.n; i++) {
        tf_item *it = &L.items[i];
        if (it->kind == IT_TEXT) {
            if (it->y != y) { y = it->y; printf("\n%5d %4d| ", it->y, it->x); }
            printf("%s%.*s ", it->node ? "_" : "", it->len, L.pool + it->text);
        } else if (verbose) {
            printf("\n  [%s %d,%d %dx%d %06x]", it->kind == IT_RECT ? "rect" : it->kind == IT_IMAGE ? "img" : it->kind == IT_FIELD ? "field" : "hline",
                   it->x, it->y, it->w, it->h, it->color);
            y = -100000;
        }
    }
    printf("\n");
    layout_clear(&L);
    style_free(st);
    dom_free(d);
    free(html);
    return 0;
}
