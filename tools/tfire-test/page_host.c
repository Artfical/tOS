#define _GNU_SOURCE
/* Runs tFire's whole engine (kernel/tfire: engine, scripts, style, layout) on the host, fetching with curl.
 *   tools/tfire-test/build_page.sh && /tmp/tfire_page https://example.com/ [width] [-v]
 * Prints the console and the laid-out page as text lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "engine.h"
#include "webfetch.h"

/* ---- what the kernel would give */
uint32_t uihost_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
void uihost_time(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    time_t now = time(0); struct tm tm; gmtime_r(&now, &tm);
    *y = tm.tm_year + 1900; *mo = tm.tm_mon + 1; *d = tm.tm_mday; *h = tm.tm_hour; *mi = tm.tm_min; *s = tm.tm_sec;
}
void serial_write(const char *s) { fputs(s, stderr); }
void csprng_fill(uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)rand(); }
#ifndef TFIRE_SHOT
int uihost_image_decode(const uint8_t *d, uint32_t n, int mw, int mh, uint8_t **px, int *w, int *h)
{
    (void)d; (void)n; (void)mw; (void)mh;
    *px = malloc(4 * 16 * 16); *w = 16; *h = 16;     /* (every picture: a 16 x 16 square) */
    return 0;
}
#endif
void *krealloc(void *p, size_t n) { return realloc(p, n); }
void tf_alloc_usage(uint32_t *s, uint32_t *b) { *s = *b = 0; }
void memory_get_usage(uint32_t *t, uint32_t *u) { *t = *u = 0; }
int uihost_fetch_poll(void *h) { webfetch_t *f = h; return f->state == WEBFETCH_RUNNING ? 0 : f->state == WEBFETCH_DONE ? 1 : -1; }

static char jar[65536];
void webfetch_cookie_set(const char *url, const char *line, int fs) { (void)url; (void)fs; const char *e = strchr(line, ';'); int n = e ? (int)(e - line) : (int)strlen(line); while (*line == ' ') { line++; n--; } if (strlen(jar) + n + 3 < sizeof(jar)) { if (jar[0]) strcat(jar, "; "); strncat(jar, line, n); } }
int webfetch_cookie_header(const char *url, char *out, int cap, int fs) { (void)url; (void)fs; strncpy(out, jar, cap - 1); out[cap - 1] = 0; return (int)strlen(out); }

webfetch_t *webfetch_request(const char *url, const char *method, const char *headers, const void *body, int blen)
{
    webfetch_t *f = calloc(1, sizeof(*f));
    strncpy(f->url, url, sizeof(f->url) - 1);
    char cmd[4096], bodyf[64] = "/tmp/tfire_req_body";
    if (body && blen) { FILE *b = fopen(bodyf, "wb"); fwrite(body, 1, blen, b); fclose(b); }
    const char *ua = getenv("TFUA") ? getenv("TFUA") : "Lynx/2.9.0 libwww-FM/2.14 tFire/1 (tOS)";
    snprintf(cmd, sizeof(cmd), "curl -s -i -L --compressed --max-time 20 -b /tmp/tfire_jar -c /tmp/tfire_jar -A '%s' %s%s%s %s%s%s -w '\\n%%{url_effective}' '%s' > /tmp/tfire_resp 2>/dev/null",
             ua, method ? "-X " : "", method ? method : "", "", body && blen ? "--data-binary @" : "", body && blen ? bodyf : "", "", url);
    if (headers) {
        /* each header line as -H */
        char h2[4096] = "";
        const char *p = headers;
        while (*p) {
            const char *e = strstr(p, "\r\n");
            if (!e) break;
            strcat(h2, " -H '"); strncat(h2, p, e - p); strcat(h2, "'");
            p = e + 2;
        }
        char c2[8192];
        snprintf(c2, sizeof(c2), "curl%s %s", h2, cmd + 4);
        strcpy(cmd, c2);
    }
    fprintf(stderr, "  [fetch %s %s]\n", method ? method : "GET", url);
    if (system(cmd) != 0) { strcpy(f->error, "curl failed"); f->state = WEBFETCH_FAILED; return f; }
    FILE *r = fopen("/tmp/tfire_resp", "rb");
    fseek(r, 0, SEEK_END); long n = ftell(r); fseek(r, 0, SEEK_SET);
    f->raw = malloc(n + 1); n = (long)fread(f->raw, 1, n, r); f->raw[n] = 0; fclose(r);
    /* the last response's headers (after redirects), the effective url on the last line */
    char *nl = memrchr(f->raw, '\n', n);
    if (nl) { strncpy(f->final_url, nl + 1, sizeof(f->final_url) - 1); *nl = 0; n = nl - (char *)f->raw; }
    char *last = (char *)f->raw;
    for (char *q = (char *)f->raw; (q = strstr(q, "HTTP/")); q++) { if (q == (char *)f->raw || q[-1] == '\n') { char *he = strstr(q, "\r\n\r\n"); if (!he) break; last = q; if (strncmp(q + 9, "3", 1) && strncmp(q + 9, "100", 3)) break; q = he; } }
    char *he = strstr(last, "\r\n\r\n");
    if (!he) { strcpy(f->error, "no response"); f->state = WEBFETCH_FAILED; return f; }
    memmove(f->raw, last, (char *)f->raw + n - last + 1);
    n -= last - (char *)f->raw;
    he = strstr((char *)f->raw, "\r\n\r\n");
    f->head_len = (uint32_t)(he + 4 - (char *)f->raw);
    f->body = f->raw + f->head_len;
    f->body_len = (uint32_t)(n - f->head_len);
    { const char *sp = strchr((char *)f->raw, ' '); f->status = sp ? atoi(sp + 1) : 0; }   /* ("HTTP/2 200" and "HTTP/1.1 200") */
    char *ct = strcasestr((char *)f->raw, "\ncontent-type:");
    if (ct && ct < he) { ct += 14; while (*ct == ' ') ct++; int i = 0; while (ct[i] && ct[i] != ';' && ct[i] != '\r' && i < 63) { f->ctype[i] = (char)(ct[i] >= 'A' && ct[i] <= 'Z' ? ct[i] + 32 : ct[i]); i++; } }
    f->state = WEBFETCH_DONE;
    return f;
}
void webfetch_free(webfetch_t *f) { if (f) { free(f->raw); free(f); } }

/* ---- fonts like the desktop's */
static int font_of(int px, int bold) { return (px > 63 ? 63 : px) | (bold ? 64 : 0); }
static int text_w(int f, const char *s, int n) { int c = 0; for (int i = 0; i < n; i++) if ((s[i] & 0xC0) != 0x80) c++; return c * (f & 63) * ((f & 64) ? 60 : 55) / 100; }
static int font_h(int f) { return (f & 63) * 5 / 4; }
static int font_ascent(int f) { return f & 63; }

#ifndef TFIRE_SHOT
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s url|file.html [width] [-v] [click:x,y] [type:text]\n", argv[0]); return 2; }
    tf_host host = { { font_of, text_w, font_h, font_ascent, 0 }, 0, 0 };
    int width = argc > 2 && atoi(argv[2]) ? atoi(argv[2]) : 900;
    int verbose = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    tf_page *p;
    if (strstr(argv[1], "://")) {
        tf_request r; memset(&r, 0, sizeof(r)); strncpy(r.url, argv[1], sizeof(r.url) - 1);
        p = page_open(&r, &host);
    } else {
        FILE *fp = fopen(argv[1], "rb"); static char buf[8 << 20]; int n = (int)fread(buf, 1, sizeof(buf) - 1, fp); buf[n] = 0;
        p = page_open_html("https://localhost/test.html", buf, n, &host);
    }
    uint32_t t0 = uihost_ms();
    int step = 3;
    for (;;) {
        page_tick(p, width, 700);
        if (!page_loading(p) && uihost_ms() - t0 > 1500) {
            /* the steps on the command line, one at a time once the page is still */
            if (step < argc) {
                const char *a = argv[step++];
                if (!strncmp(a, "click:", 6)) { int x, y; sscanf(a + 6, "%d,%d", &x, &y); fprintf(stderr, "  [click %d,%d]\n", x, y); page_click(p, x, y); t0 = uihost_ms(); }
                else if (!strncmp(a, "type:", 5)) { for (const char *c = a + 5; *c; c++) page_key(p, *c == '~' ? '\n' : (unsigned char)*c); t0 = uihost_ms(); }
                continue;
            }
            break;
        }
        if (uihost_ms() - t0 > 60000) { fprintf(stderr, "  [still loading after 60 s]\n"); break; }
        tf_request nav;
        if (page_take_navigation(p, &nav)) {
            fprintf(stderr, "  [navigate %s %s%s%s]\n", nav.method[0] ? nav.method : "GET", nav.url, nav.body ? " body=" : "", nav.body ? nav.body : "");
            page_close(p);
            p = page_open(&nav, &host);
            free(nav.body);
            t0 = uihost_ms();
        }
        usleep(2000);
    }
    tf_layout *L = page_layout(p);
    printf("url: %s\ntitle: [%s]\nitems %d doc %dx%d\n", page_url(p), page_title(p), L->n, L->doc_w, L->doc_h);
    int y = -100000;
    for (int i = 0; i < L->n; i++) {
        tf_item *it = &L->items[i];
        if (it->kind == IT_TEXT) {
            if (it->y != y) { y = it->y; printf("\n%5d %4d| ", it->y, it->x); }
            printf("%.*s ", it->len, L->pool + it->text);
        } else if (it->kind == IT_FIELD) {
            printf("[%.*s]%s", it->len, L->pool + it->text, verbose ? "" : " ");
            if (verbose) printf("@%d,%d ", it->x, it->y);
        } else if (verbose && it->kind == IT_FIELD) printf("@%d,%d ", it->x, it->y);
        else if (verbose && it->kind == IT_RECT) printf("\n  [rect %d,%d %dx%d %06x]", it->x, it->y, it->w, it->h, it->color), y = -100000;
        else if (it->kind == IT_IMAGE) printf("<img %dx%d> ", it->w, it->h);
    }
    printf("\n");
    page_close(p);
    return 0;
}
#endif
