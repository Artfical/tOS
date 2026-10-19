/* Runs tFire's HTML parser and layout on the host: prints the runs and the laid-out lines.
 *   cc -I../../kernel/display -I../../kernel/lib host.c -o /tmp/tfire_host && /tmp/tfire_host page.html */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define free free
#include "../../kernel/display/apps/tfire.c"

/* what the desktop would give it */
const ui_font_t ui_font_regular = { 14, 14, 4, 20, 0, 0 }, ui_font_medium = { 14, 14, 4, 20, 0, 0 },
                ui_font_small = { 12, 12, 3, 17, 0, 0 }, ui_font_title = { 22, 22, 6, 31, 0, 0 }, ui_font_huge = { 48, 47, 12, 67, 0, 0 };
ui_theme_t ui_theme;
int gfx_text_width(const ui_font_t *f, const char *s) { int n = 0; for (; *s; s++) if ((*s & 0xC0) != 0x80) n++; return n * f->size * 55 / 100; }
void *krealloc(void *p, size_t n) { return realloc(p, n); }

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "rb");
    static char buf[8 << 20];
    int n = (int)fread(buf, 1, sizeof(buf), fp);
    static tf_t t;
    parse_html(&t, buf, (uint32_t)n);
    printf("title: [%s] runs %d links %d\n", t.title, t.nruns, t.nlinks);
    int verbose = argc > 2;
    for (int i = 0; verbose && i < t.nruns; i++)
        printf("run brk%d st%02x ind%d bul%d link%d: [%.*s]\n", t.runs[i].brk, t.runs[i].style, t.runs[i].indent, t.runs[i].bullet,
               t.runs[i].link, (int)t.runs[i].len, t.pool + t.runs[i].off);
    layout(&t, 900);
    int y = -1;
    for (int i = 0; i < t.nboxes; i++) {
        if (t.boxes[i].y != y) { y = t.boxes[i].y; printf("\n%5d %3d| ", y, t.boxes[i].x); }
        printf("%s%.*s ", t.boxes[i].link >= 0 ? "_" : "", t.boxes[i].len, t.pool + t.boxes[i].off);
    }
    printf("\n doc_h %d\n", t.doc_h);
    return 0;
}
