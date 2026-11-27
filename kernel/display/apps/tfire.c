#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"
#include "engine.h"
#include "keyboard.h"

#define realloc krealloc          /* (the kernel's own names) */
#define calloc kcalloc

/*
 * tFire, the web browser (Project tFire, update 6: type, shadows and gradients).
 *
 * The window around the engine (kernel/tfire): the toolbar, the address bar and the history are here; the page
 * itself (HTML parsed by NetSurf's libhubbub, CSS by libcss, scripts by QuickJS, tFire's own layout) comes from
 * engine.c as a list of things to draw, which this draws with the desktop's fonts.
 */

#define TB_H     46              /* toolbar */
#define TAB_H    34              /* the tab strip (shown when there are two tabs or more) */
#define DL_H     40              /* the download bar, along the top of the page */
#define DL_SHOW_X  184           /* its buttons, from the right edge */
#define DL_CLOSE_X 44
#define FIND_H   38              /* the find bar, along the bottom */
#define SUG_ROW  44              /* a row of the address bar's suggestions */
#define SUG_MAX  6
#define STAR_W   32              /* the bookmark star at the right end of the address bar */
#define MAX_TABS 8
#define CLOSED_MAX 8
#define URL_MAX  1024
#define HIST_MAX 32
#define SEARCH   "https://html.duckduckgo.com/html/?q="
#define HOME     "tfire:home"

void tf_free(void *p);           /* (what the engine allocates, it frees: tfire/libc/tf_alloc.c) */
extern const char tfire_notices[];   /* THIRD_PARTY_NOTICES.html, the licences (tfire_notices.s) */

/* what belongs to one tab: its page and its way back and forward. The window keeps the current tab's here (as t->url,
 * t->page...) and the others in saved[] (a switch copies them over). */
#define TAB_FIELDS \
    char url[URL_MAX];            /* the page shown (or being loaded) */ \
    char hist[HIST_MAX][URL_MAX]; \
    int nhist, hpos; \
    tf_page *page; \
    uint32_t load_start; \
    int scroll; \
    /* the page that was shown, kept on screen until the next one's answer is here (and back if that is a download) */ \
    tf_page *prev; \
    char prev_url[URL_MAX]; \
    int prev_hpos, prev_nhist; \
    char title[96]; \
    int recorded;                 /* the page is in the history */

typedef struct { TAB_FIELDS } tab_t;

typedef struct {
    union { tab_t tab; struct { TAB_FIELDS }; };
    char field[URL_MAX];          /* the address bar */
    int editing, select_all, caret;
    int view_w, view_h;           /* the page's part of the window, when last drawn */
    uint32_t blink;
    uiapp_t *app;
    char status[URL_MAX];         /* the link under the pointer, shown at the bottom */
    /* the last download, in a bar at the bottom */
    char dl_text[256];
    char dl_dir[200];
    int dl_ok;
    /* tabs: saved[i] holds every tab but the current one (cur) */
    tab_t *saved[MAX_TABS];
    int ntabs, cur;
    char closed[CLOSED_MAX][URL_MAX];     /* what Ctrl+Shift+T opens again, the last one first */
    int nclosed;
    /* find in page */
    int find_on, find_focus, find_dirty, find_last_n;
    char find_q[64];
    int find_len, find_n, find_i;
    /* what the address bar suggests while it is typed in */
    int sug_n, sug_sel;
    char sug_url[SUG_MAX][256];
    char sug_title[SUG_MAX][80];
    char sug_typed[URL_MAX];
    char toast[64];               /* a short message over the page */
    uint32_t toast_until;
} tf_t;

/* the page on screen: the one that was there, while the new one is still coming */
static tf_page *shown(tf_t *t) { return t->prev ? t->prev : t->page; }

/* ------------------------------------------------------------------ fonts for the engine */

/* tFire's own fonts (ui_assets: many sizes of Inter, and JetBrains Mono): the nearest size of the kind asked for */
#define fonts tfire_fonts

static int font_of(int px, int flags)
{
    int kind = (flags & 2) ? 2 : (flags & 1), best = 0, bd = 1 << 30;
    for (int i = 0; i < tfire_font_count; i++) {
        if (tfire_font_kind[i] != kind) continue;
        int d = tfire_font_size[i] - px;
        d = d < 0 ? -d * 2 + 1 : d * 2;                  /* (between two: the smaller one) */
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int text_w(int f, const char *s, int n)
{
    char tmp[256];
    int w = 0;
    while (n > 0) {
        int k = n > 255 ? 255 : n;
        memcpy(tmp, s, (size_t)k);
        tmp[k] = 0;
        w += gfx_text_width(fonts[f], tmp);
        s += k;
        n -= k;
    }
    return w;
}

static int font_h(int f) { return fonts[f]->ascent + fonts[f]->descent; }
static int font_ascent(int f) { return fonts[f]->ascent; }

static void alert_cb(void *ctx, const char *msg)
{
    (void)ctx;
    uihost_notify("tfire", "tfire", "This page says", msg, 0, 0);
}

static const tf_host host = { { font_of, text_w, font_h, font_ascent, 0 }, alert_cb, 0 };

/* ------------------------------------------------------------------ addresses */

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* appends src to dst (cap bytes in all), cut short if it must be */
static void strncat_(char *dst, const char *src, int cap)
{
    int l = (int)strlen(dst);
    while (*src && l < cap - 1) dst[l++] = *src++;
    dst[l] = 0;
}

/* what was typed in the address bar -> an address: a web address as it is, a name like "example.com"
 * gets https://, anything else is a search */
static void typed_to_url(const char *in, char *out)
{
    while (*in == ' ') in++;
    if (starts(in, "http://") || starts(in, "https://") || starts(in, "tfire:")) { strncpy(out, in, URL_MAX - 1); out[URL_MAX - 1] = 0; return; }
    int dot = 0, space = 0;
    for (const char *c = in; *c; c++) { if (*c == '.') dot = 1; if (*c == ' ') space = 1; }
    if (dot && !space) {
        strcpy(out, "https://");
        strncpy(out + 8, in, URL_MAX - 9);
        out[URL_MAX - 1] = 0;
        return;
    }
    strcpy(out, SEARCH);
    int o = (int)strlen(out);
    for (const char *c = in; *c && o < URL_MAX - 4; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == ' ') out[o++] = '+';
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') out[o++] = (char)ch;
        else { out[o++] = '%'; out[o++] = "0123456789ABCDEF"[ch >> 4]; out[o++] = "0123456789ABCDEF"[ch & 15]; }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ tFire's own page */

static const char home_head[] =
    "<!doctype html><html><head><title>tFire</title><style>"
    "body{font-family:sans-serif;margin:0;background:#f7f7fa;color:#1f2937}"
    ".top{background:linear-gradient(#ff7a18,#e8461e);background-color:#f0582a;color:#fff;padding:36px 48px 28px}"
    ".top h1{margin:0;font-size:40px}.top p{margin:6px 0 0;color:#ffe9dc}"
    ".wrap{padding:24px 48px}"
    ".card{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:14px 18px;margin:0 0 12px}"
    ".card a{font-weight:bold;color:#1a56db;text-decoration:none}.card span{color:#6b7280}"
    "h3{margin:22px 0 10px;color:#374151}ul{margin:0;padding-left:22px}li{margin:4px 0}"
    "#clock{color:#6b7280;font-size:13px}"
    "</style></head><body>"
    "<div class=top><h1>tFire</h1><p>Project tFire, update 6: type, shadows and gradients. CSS, JavaScript and the web as it is.</p></div>"
    "<div class=wrap>"
    "<p id=clock>&nbsp;</p>";

static const char home_cards[] =
    "<div class=card><a href=\"https://en.wikipedia.org/wiki/Firefox\">Wikipedia</a> <span>&mdash; Firefox, with its style sheets and pictures</span></div>"
    "<div class=card><a href=\"https://news.ycombinator.com/\">Hacker News</a> <span>&mdash; tables, forms, scripts</span></div>"
    "<div class=card><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo</a> <span>&mdash; search the web</span></div>"
    "<div class=card><a href=\"https://www.bing.com/\">Bing</a> <span>&mdash; search, with its scripts</span></div>"
    "<div class=card><a href=\"https://text.npr.org/\">NPR News</a> <span>&mdash; the news</span></div>"
    "<div class=card><a href=\"https://update.artfical.com/\">tOS updates</a></div>";

static const char home_tail[] =
    "<h3>The engine</h3><ul>"
    "<li>HTML5 parsing (NetSurf's libhubbub), CSS 2.1 and much of CSS 3 (libcss): selectors, the cascade, media queries</li>"
    "<li>Layout: blocks, inline text, tables, flex rows, lists, form fields, pictures (PNG, JPEG)</li>"
    "<li>JavaScript: QuickJS (ES2023) with the DOM, events, timers, fetch, XMLHttpRequest, cookies and localStorage</li>"
    "<li>Forms: typing, check boxes, lists, GET and POST</li></ul>"
    "<p id=js style=\"color:#9ca3af\">JavaScript is off.</p>"
    "<p><a href=\"tfire:bookmarks\">Bookmarks</a> &middot; <a href=\"tfire:history\">History</a> &middot; <a href=\"tfire:licenses\">Licences</a> &mdash; the free software tFire is built from</p>"
    "</div>"
    "<script>"
    "const js = document.getElementById('js');"
    "js.textContent = 'JavaScript is on: ' + navigator.userAgent;"
    "js.style.color = '#059669';"
    "function tick() { document.getElementById('clock').textContent = new Date().toUTCString(); }"
    "tick(); setInterval(tick, 1000);"
    "</script></body></html>";

/* ------------------------------------------------------------------ small things */

/* tOS has no clipboard shared between apps: Ctrl+C and Ctrl+V work between tFire's own windows */
static char tf_clip[URL_MAX];
static void uihost_clipboard_set(const char *text)
{
    strncpy(tf_clip, text ? text : "", sizeof(tf_clip) - 1);
    tf_clip[sizeof(tf_clip) - 1] = 0;
}

static int lower_(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* is needle in hay, ignoring the case of ASCII letters */
static int ci_find(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++) {
        int k = 0;
        while (k < nl && hay[k] && lower_((unsigned char)hay[k]) == lower_((unsigned char)needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int ci_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) if (lower_((unsigned char)*s) != lower_((unsigned char)*p)) return 0;
    return 1;
}

static int parse_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0');
    return v;
}

static char *xdupn(const char *s, int n)
{
    char *d = malloc((size_t)n + 1);
    if (d) { memcpy(d, s, (size_t)n); d[n] = 0; }
    return d;
}

/* one code point as UTF-8: the bytes it takes (1 to 4) */
static int utf8_enc(int k, char *out)
{
    if (k < 0x80) { out[0] = (char)k; return 1; }
    if (k < 0x800) { out[0] = (char)(0xC0 | (k >> 6)); out[1] = (char)(0x80 | (k & 0x3F)); return 2; }
    if (k < 0x10000) { out[0] = (char)(0xE0 | (k >> 12)); out[1] = (char)(0x80 | ((k >> 6) & 0x3F)); out[2] = (char)(0x80 | (k & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (k >> 18)); out[1] = (char)(0x80 | ((k >> 12) & 0x3F)); out[2] = (char)(0x80 | ((k >> 6) & 0x3F)); out[3] = (char)(0x80 | (k & 0x3F));
    return 4;
}

/* a string that grows (for the pages tFire writes itself) */
typedef struct { char *p; int len, cap, bad; } sb_t;

static void sb_add_n(sb_t *b, const char *s, int n)
{
    if (b->bad) return;
    if (b->len + n + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, (size_t)nc);
        if (!np) { b->bad = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}
static void sb_add(sb_t *b, const char *s) { sb_add_n(b, s, (int)strlen(s)); }

/* text for an HTML page: what a page's own title could hold must not become markup */
static void sb_esc(sb_t *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_add(b, "&amp;"); break;
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '"': sb_add(b, "&quot;"); break;
        default: sb_add_n(b, s, 1); break;
        }
    }
}

static void sb_int(sb_t *b, int v)
{
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) sb_add_n(b, &d[--n], 1);
}

/* ------------------------------------------------------------------ bookmarks and history (kept in ~/.tfire) */

#define BM_MAX 100
#define HS_MAX 300
typedef struct { char *url, *title; } site_t;
static site_t bm[BM_MAX], hs[HS_MAX];
static int nbm, nhs, store_ready;

static void store_dir(char *out, int cap)
{
    strncpy(out, uihost_home(), (size_t)cap - 1);
    out[cap - 1] = 0;
    if (strcmp(out, "/")) strncat_(out, "/", cap);
    strncat_(out, ".tfire", cap);
}

static void store_path(const char *name, char *out, int cap)
{
    store_dir(out, cap);
    strncat_(out, "/", cap);
    strncat_(out, name, cap);
}

/* a page's title for the files: one line, not too long */
static char *clean_title(const char *s)
{
    int n = (int)strlen(s);
    if (n > 200) { n = 200; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    char *d = malloc((size_t)n + 1);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = (unsigned char)s[i] < 32 ? ' ' : s[i];
    d[n] = 0;
    return d;
}

static void site_free(site_t *s) { free(s->url); free(s->title); s->url = s->title = 0; }

static void site_load(const char *name, site_t *arr, int *n, int max)
{
    char path[520];
    store_path(name, path, sizeof(path));
    uint8_t *d = 0;
    uint32_t len = 0;
    if (uihost_read(path, &d, &len) != 0 || !d) return;
    uint32_t i = 0;
    while (i < len && *n < max) {
        uint32_t e = i;
        while (e < len && d[e] != '\n') e++;
        uint32_t tab = i;
        while (tab < e && d[tab] != '\t') tab++;
        if (tab > i && tab - i < URL_MAX) {
            char *u = xdupn((const char *)d + i, (int)(tab - i));
            char *t = tab < e ? xdupn((const char *)d + tab + 1, (int)(e - tab - 1 > 200 ? 200 : e - tab - 1)) : xdupn("", 0);
            if (u && t) { arr[*n].url = u; arr[*n].title = t; (*n)++; }
            else { free(u); free(t); }
        }
        i = e + 1;
    }
    free(d);
}

static void site_save(const char *name, site_t *arr, int n)
{
    sb_t b = { 0 };
    for (int i = 0; i < n; i++) {
        sb_add(&b, arr[i].url);
        sb_add(&b, "\t");
        sb_add(&b, arr[i].title);
        sb_add(&b, "\n");
    }
    if (!b.bad) {
        char dir[520], path[520];
        store_dir(dir, sizeof(dir));
        uihost_mkdir(dir);
        store_path(name, path, sizeof(path));
        uihost_write(path, b.p ? b.p : "", (uint32_t)b.len);
    }
    free(b.p);
}

static void store_init(void)
{
    if (store_ready) return;
    store_ready = 1;
    site_load("bookmarks.txt", bm, &nbm, BM_MAX);
    site_load("history.txt", hs, &nhs, HS_MAX);
}

static int recordable(const char *u) { return starts(u, "http://") || starts(u, "https://"); }

static void hs_record(const char *url, const char *title)
{
    store_init();
    if (!recordable(url)) return;
    for (int i = 0; i < nhs; i++)
        if (!strcmp(hs[i].url, url)) {
            site_free(&hs[i]);
            memmove(&hs[i], &hs[i + 1], (size_t)(nhs - i - 1) * sizeof(site_t));
            nhs--;
            break;
        }
    if (nhs == HS_MAX) { site_free(&hs[nhs - 1]); nhs--; }
    char *u = xdupn(url, (int)strlen(url)), *t = clean_title(title[0] ? title : url);
    if (!u || !t) { free(u); free(t); return; }
    memmove(&hs[1], &hs[0], (size_t)nhs * sizeof(site_t));
    hs[0].url = u;
    hs[0].title = t;
    nhs++;
    site_save("history.txt", hs, nhs);
}

static void hs_remove(int i)
{
    store_init();
    if (i < 0 || i >= nhs) return;
    site_free(&hs[i]);
    memmove(&hs[i], &hs[i + 1], (size_t)(nhs - i - 1) * sizeof(site_t));
    nhs--;
    site_save("history.txt", hs, nhs);
}

static void hs_clear(void)
{
    store_init();
    for (int i = 0; i < nhs; i++) site_free(&hs[i]);
    nhs = 0;
    site_save("history.txt", hs, nhs);
}

static int bm_find(const char *url)
{
    store_init();
    for (int i = 0; i < nbm; i++) if (!strcmp(bm[i].url, url)) return i;
    return -1;
}

static void bm_remove(int i)
{
    store_init();
    if (i < 0 || i >= nbm) return;
    site_free(&bm[i]);
    memmove(&bm[i], &bm[i + 1], (size_t)(nbm - i - 1) * sizeof(site_t));
    nbm--;
    site_save("bookmarks.txt", bm, nbm);
}

/* 1 added, 0 removed, -1 no room (or not a page that can be kept) */
static int bm_toggle(const char *url, const char *title)
{
    int i = bm_find(url);
    if (i >= 0) { bm_remove(i); return 0; }
    if (nbm >= BM_MAX || !(recordable(url) || url[0] == '/')) return -1;
    char *u = xdupn(url, (int)strlen(url)), *t = clean_title(title[0] ? title : url);
    if (!u || !t) { free(u); free(t); return -1; }
    bm[nbm].url = u;
    bm[nbm].title = t;
    nbm++;
    site_save("bookmarks.txt", bm, nbm);
    return 1;
}

/* ------------------------------------------------------------------ the pages tFire writes: home, bookmarks, history */

static void page_head(sb_t *b, const char *title)
{
    sb_add(b, "<!doctype html><html><head><meta charset=utf-8><title>");
    sb_esc(b, title);
    sb_add(b, "</title><style>body{font-family:sans-serif;margin:0;background:#f7f7fa;color:#1f2937}"
              ".top{background:linear-gradient(#ff7a18,#e8461e);background-color:#f0582a;color:#fff;padding:26px 48px 20px}"
              ".top h1{margin:0;font-size:32px}.top p{margin:4px 0 0;color:#ffe9dc}"
              ".wrap{padding:20px 48px}.row{background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:10px 16px;margin:0 0 8px}"
              ".row a{color:#1a56db;text-decoration:none}.u{color:#9ca3af;font-size:13px;display:block}"
              ".row a.x,a.x{color:#b91c1c;font-size:13px}.m{color:#6b7280}.nav a{color:#fff}"
              "</style></head><body><div class=top><h1>");
    sb_esc(b, title);
    sb_add(b, "</h1><p class=nav><a href=\"tfire:home\">Home</a> &middot; <a href=\"tfire:bookmarks\">Bookmarks</a> &middot; <a href=\"tfire:history\">History</a> &middot; <a href=\"tfire:downloads\">Downloads</a></p></div><div class=wrap>");
}

static void site_rows(sb_t *b, site_t *arr, int n, const char *del)
{
    for (int i = 0; i < n; i++) {
        sb_add(b, "<div class=row><a href=\"");
        sb_esc(b, arr[i].url);
        sb_add(b, "\">");
        sb_esc(b, arr[i].title);
        sb_add(b, "</a><span class=u>");
        sb_esc(b, arr[i].url);
        sb_add(b, "</span><a class=x href=\"");
        sb_add(b, del);
        sb_int(b, i);
        sb_add(b, "\">Remove</a></div>");
    }
}

static void bookmarks_html(sb_t *b)
{
    store_init();
    page_head(b, "Bookmarks");
    if (!nbm) sb_add(b, "<p class=m>No bookmarks yet. Press Ctrl+D on a page, or click the star in the address bar.</p>");
    site_rows(b, bm, nbm, "tfire:bm-del:");
    sb_add(b, "</div></body></html>");
}

static void history_html(sb_t *b)
{
    store_init();
    page_head(b, "History");
    if (!nhs) sb_add(b, "<p class=m>Nothing here yet: the pages you visit are listed here, the newest first.</p>");
    else sb_add(b, "<p><a class=x href=\"tfire:hs-clear\">Clear all history</a></p>");
    site_rows(b, hs, nhs, "tfire:hs-del:");
    sb_add(b, "</div></body></html>");
}

/* the Downloads page: what is in ~/Downloads, with its size, to open or remove */
static char dl_dir[200];

static void dl_path(char *out)
{
    strcpy(out, uihost_home());
    if (strcmp(out, "/")) strcat(out, "/");
    strcat(out, "Downloads");
}

static const char *size_str(uint32_t n, char *buf)
{
    if (n >= 1024u * 1024u) { int o = 0, x = n / (1024 * 1024), f = (n % (1024 * 1024)) * 10 / (1024 * 1024); if (x >= 10) { int t[8],c=0; while(x){t[c++]='0'+x%10;x/=10;} while(c) buf[o++]=t[--c]; } else { buf[o++]='0'+x; buf[o++]='.'; buf[o++]='0'+f; } buf[o]=0; strcat(buf, " MB"); }
    else if (n >= 1024u) { int t[8],c=0,x=n/1024; while(x){t[c++]='0'+x%10;x/=10;} int o=0; while(c) buf[o++]=t[--c]; buf[o]=0; strcat(buf, " KB"); }
    else { int t[8],c=0,x=n,o=0; if(!x) buf[o++]='0'; while(x){t[c++]='0'+x%10;x/=10;} while(c) buf[o++]=t[--c]; buf[o]=0; strcat(buf, " B"); }
    return buf;
}

static void downloads_html(sb_t *b)
{
    dl_path(dl_dir);
    page_head(b, "Downloads");
    static uihost_entry_t e[128];
    int n = uihost_list(dl_dir, e, 128);
    if (n <= 0) sb_add(b, "<p class=m>No downloads yet. Files you download land in your Downloads folder.</p>");
    else {
        sb_add(b, "<p><a class=x href=\"tfire:dl-clear\">Clear all</a></p>");
        for (int i = 0; i < n; i++) {
            if (e[i].name[0] == '.') continue;
            char sz[24]; size_str(e[i].size, sz);
            sb_add(b, "<div class=row><a href=\"tfire:dl-open:");
            sb_int(b, i);
            sb_add(b, "\">");
            sb_esc(b, e[i].name);
            sb_add(b, "</a><span class=u>");
            sb_esc(b, sz);
            sb_add(b, "</span><a class=x href=\"tfire:dl-del:");
            sb_int(b, i);
            sb_add(b, "\">Remove</a></div>");
        }
    }
    sb_add(b, "</div></body></html>");
}

static void home_html(sb_t *b)
{
    store_init();
    sb_add(b, home_head);
    if (nbm) {
        sb_add(b, "<h3>Bookmarks</h3>");
        for (int i = 0; i < nbm && i < 12; i++) {
            sb_add(b, "<div class=card><a href=\"");
            sb_esc(b, bm[i].url);
            sb_add(b, "\">");
            sb_esc(b, bm[i].title);
            sb_add(b, "</a></div>");
        }
    } else sb_add(b, home_cards);
    if (nhs) {
        sb_add(b, "<h3>Recently visited</h3><ul>");
        for (int i = 0; i < nhs && i < 8; i++) {
            sb_add(b, "<li><a href=\"");
            sb_esc(b, hs[i].url);
            sb_add(b, "\">");
            sb_esc(b, hs[i].title);
            sb_add(b, "</a></li>");
        }
        sb_add(b, "</ul>");
    }
    sb_add(b, home_tail);
}

/* ------------------------------------------------------------------ loading */

/* the page for a request: tFire's own pages are made here, the rest are fetched */
static tf_page *open_url(const tf_request *req)
{
    const char *u = req->url;
    if (!strcmp(u, "tfire:licenses")) return page_open_html(u, tfire_notices, (int)strlen(tfire_notices), &host);
    if (!strcmp(u, "tfire:downloads")) {
        sb_t b = { 0 };
        downloads_html(&b);
        tf_page *p = 0;
        if (b.p && !b.bad) p = page_open_html(u, b.p, b.len, &host);
        free(b.p);
        return p;
    }
    if (!strcmp(u, HOME) || !strcmp(u, "tfire:bookmarks") || !strcmp(u, "tfire:history")) {
        sb_t b = { 0 };
        if (!strcmp(u, HOME)) home_html(&b);
        else if (!strcmp(u, "tfire:bookmarks")) bookmarks_html(&b);
        else history_html(&b);
        tf_page *p = 0;
        if (b.p && !b.bad) p = page_open_html(u, b.p, b.len, &host);
        free(b.p);
        return p;
    }
    return page_open(req, &host);
}

/* ------------------------------------------------------------------ the window */

/* keys the desktop version adds (platform/main.c): the shortcuts of any browser */
#define TF_KEY_URL      0x200001      /* Ctrl+L */
#define TF_KEY_RELOAD   0x200002      /* Ctrl+R, F5 */
#define TF_KEY_BACK     0x200003      /* Alt+Left, the mouse's back button */
#define TF_KEY_FORWARD  0x200004      /* Alt+Right */
#define TF_KEY_NEWTAB   0x200005      /* Ctrl+T */
#define TF_KEY_CLOSETAB 0x200006      /* Ctrl+W */
#define TF_KEY_NEXTTAB  0x200007      /* Ctrl+Tab, Ctrl+PageDown */
#define TF_KEY_PREVTAB  0x200008      /* Ctrl+Shift+Tab, Ctrl+PageUp */
#define TF_KEY_REOPEN   0x200009      /* Ctrl+Shift+T */
#define TF_KEY_FIND     0x20000A      /* Ctrl+F */
#define TF_KEY_BOOKMARK 0x20000B      /* Ctrl+D */
#define TF_KEY_HISTORY  0x20000C      /* Ctrl+H */
#define TF_KEY_BOOKMARKS 0x20000D     /* Ctrl+Shift+O */
#define TF_KEY_SELALL   0x20000E      /* Ctrl+A */
#define TF_KEY_COPY     0x20000F      /* Ctrl+C */
#define TF_KEY_CUT      0x200010      /* Ctrl+X */
#define TF_KEY_SENTER   0x200011      /* Shift+Enter */
#define TF_KEY_HOME     0x200012
#define TF_KEY_END      0x200013
#define TF_KEY_PGUP     0x200014
#define TF_KEY_PGDN     0x200015
#define TF_KEY_PASTE    0x200016      /* Ctrl+V */
#define TF_KEY_TAB1     0x200020      /* Ctrl+1 .. Ctrl+9: that tab (9: the last) */

static void toast(tf_t *t, const char *msg)
{
    strncpy(t->toast, msg, sizeof(t->toast) - 1);
    t->toast[sizeof(t->toast) - 1] = 0;
    t->toast_until = uihost_ms() + 1800;
}

static void set_title(uiapp_t *a, tf_t *t)
{
    const char *pt = t->page ? page_title(t->page) : "";
    strncpy(t->title, pt, sizeof(t->title) - 1);
    t->title[sizeof(t->title) - 1] = 0;
    strcpy(a->title, "tFire");
    if (pt[0] && strcmp(pt, "tFire")) {
        strcat(a->title, " \xE2\x80\x94 ");
        int l = (int)strlen(a->title);
        strncpy(a->title + l, pt, sizeof(a->title) - (size_t)l - 1);
        a->title[sizeof(a->title) - 1] = 0;
    }
}

/* the address bar shows the current page's address, the caret at its end */
static void field_reset(tf_t *t)
{
    strcpy(t->field, t->url);
    t->caret = (int)strlen(t->field);
    t->sug_n = 0;
    t->sug_sel = -1;
}

static void go(uiapp_t *a, tf_t *t, const char *url, int record);

/* shows req; record = a new entry in the history */
static void go_req(uiapp_t *a, tf_t *t, const tf_request *req, int record)
{
    if (!strcmp(req->url, "tfire:back") || !strcmp(req->url, "tfire:forward")) {
        int d = req->url[6] == 'b' ? -1 : 1;
        if (t->hpos + d >= 0 && t->hpos + d < t->nhist) {
            t->hpos += d;
            tf_request r;
            memset(&r, 0, sizeof(r));
            strcpy(r.url, t->hist[t->hpos]);
            go_req(a, t, &r, 0);
        }
        return;
    }
    /* the Downloads page: open a file, remove one, or clear them all (only from tFire's own Downloads page) */
    if (starts(req->url, "tfire:dl-open:") || starts(req->url, "tfire:dl-del:") || !strcmp(req->url, "tfire:dl-clear")) {
        if (!strcmp(t->url, "tfire:downloads")) {
            dl_path(dl_dir);
            static uihost_entry_t e[128];
            int n = uihost_list(dl_dir, e, 128);
            const char *num = req->url;
            for (const char *q = req->url; *q; q++) if (*q == ':') num = q + 1;   /* the number after the last : */
            int act = req->url[9];                                               /* o open, d delete, c clear */
            if (n > 0 && act == 'o') {
                int i = parse_int(num);
                if (i >= 0 && i < n) { char p[260]; strcpy(p, dl_dir); strcat(p, "/"); strcat(p, e[i].name); uihost_open("files", p); }
            } else if (n > 0 && act == 'd') {
                int i = parse_int(num);
                if (i >= 0 && i < n) { char p[260]; strcpy(p, dl_dir); strcat(p, "/"); strcat(p, e[i].name); uihost_delete(p); }
                go(a, t, "tfire:downloads", 0);
            } else if (act == 'c') {
                for (int i = 0; i < n; i++) { if (e[i].name[0] == '.') continue; char p[260]; strcpy(p, dl_dir); strcat(p, "/"); strcat(p, e[i].name); uihost_delete(p); }
                go(a, t, "tfire:downloads", 0);
            }
        }
        return;
    }
    /* removing a bookmark or a history entry: only from tFire's own pages for them (any web page can link to tfire:) */
    if (starts(req->url, "tfire:bm-del:") || starts(req->url, "tfire:hs-del:") || !strcmp(req->url, "tfire:hs-clear")) {
        int from_bm = !strcmp(t->url, "tfire:bookmarks"), from_hs = !strcmp(t->url, "tfire:history");
        if (req->url[6] == 'b' && from_bm) { bm_remove(parse_int(req->url + 13)); go(a, t, "tfire:bookmarks", 0); }
        else if (req->url[6] == 'h' && from_hs) {
            if (req->url[9] == 'c') hs_clear();
            else hs_remove(parse_int(req->url + 13));
            go(a, t, "tfire:history", 0);
        }
        return;
    }
    if (t->page) {
        if (t->prev) page_close(t->prev);                    /* (one still waiting for its answer: gone) */
        else { strcpy(t->prev_url, t->url); t->prev_hpos = t->hpos; t->prev_nhist = t->nhist; }
        t->prev = t->page;
        t->page = 0;
    }
    strncpy(t->url, req->url, URL_MAX - 1);
    t->url[URL_MAX - 1] = 0;
    field_reset(t);
    t->editing = 0;
    t->recorded = 0;
    t->find_dirty = 1;
    if (!t->prev) t->scroll = 0;
    if (record && !req->replace) {
        if (t->hpos < t->nhist - 1) t->nhist = t->hpos + 1;
        if (t->nhist == HIST_MAX) { for (int i = 1; i < HIST_MAX; i++) strcpy(t->hist[i - 1], t->hist[i]); t->nhist--; }
        strcpy(t->hist[t->nhist], t->url);
        t->hpos = t->nhist++;
    } else if (req->replace && t->hpos >= 0 && t->hpos < t->nhist) strcpy(t->hist[t->hpos], t->url);
    t->page = open_url(req);
    t->load_start = uihost_ms();
    set_title(a, t);
    a->dirty = 1;
}

static void go(uiapp_t *a, tf_t *t, const char *url, int record)
{
    tf_request r;
    memset(&r, 0, sizeof(r));
    strncpy(r.url, url, URL_MAX - 1);
    go_req(a, t, &r, record);
}

/* ------------------------------------------------------------------ tabs */

static int tabs_h(tf_t *t) { return t->ntabs > 1 ? TAB_H : 0; }

static void tab_free(tab_t *s)
{
    if (s->page) page_close(s->page);
    if (s->prev) page_close(s->prev);
    s->page = s->prev = 0;
}

/* the current tab's state moved to saved[cur]: 1 = done */
static int tab_stash(tf_t *t)
{
    tab_t *s = malloc(sizeof(tab_t));
    if (!s) return 0;
    memcpy(s, &t->tab, sizeof(tab_t));
    t->saved[t->cur] = s;
    return 1;
}

/* saved[i] becomes the current tab (the current one has been stashed, or is gone) */
static void tab_load(uiapp_t *a, tf_t *t, int i)
{
    tab_t *s = t->saved[i];
    memcpy(&t->tab, s, sizeof(tab_t));
    free(s);
    t->saved[i] = 0;
    t->cur = i;
    field_reset(t);
    t->editing = 0;
    t->status[0] = 0;
    t->find_dirty = 1;
    set_title(a, t);
    a->dirty = 1;
}

static void tab_switch(uiapp_t *a, tf_t *t, int i)
{
    if (i < 0 || i >= t->ntabs || i == t->cur) return;
    if (!tab_stash(t)) return;
    tab_load(a, t, i);
}

/* a new tab at the end: in front (it is shown) or behind (it loads in the background) */
static void tab_new(uiapp_t *a, tf_t *t, const char *url, int front)
{
    if (t->ntabs >= MAX_TABS) { toast(t, "Too many tabs"); return; }
    if (front) {
        if (!tab_stash(t)) return;
        t->saved[t->ntabs] = 0;
        t->cur = t->ntabs++;
        memset(&t->tab, 0, sizeof(tab_t));
        t->hpos = -1;
        t->status[0] = 0;
        go(a, t, url, 1);
        return;
    }
    tab_t *n = calloc(1, sizeof(tab_t));
    if (!n) return;
    tf_request r;
    memset(&r, 0, sizeof(r));
    strncpy(r.url, url, URL_MAX - 1);
    strcpy(n->url, r.url);
    strcpy(n->hist[0], r.url);
    n->nhist = 1;
    n->hpos = 0;
    n->page = open_url(&r);
    n->load_start = uihost_ms();
    t->saved[t->ntabs++] = n;
    a->dirty = 1;
}

static void tab_close(uiapp_t *a, tf_t *t, int i)
{
    if (i < 0 || i >= t->ntabs) return;
    const char *u = i == t->cur ? t->url : t->saved[i]->url;
    if (u[0] && strcmp(u, HOME)) {                               /* Ctrl+Shift+T brings it back */
        for (int k = CLOSED_MAX - 1; k > 0; k--) strcpy(t->closed[k], t->closed[k - 1]);
        strncpy(t->closed[0], u, URL_MAX - 1);
        t->closed[0][URL_MAX - 1] = 0;
        if (t->nclosed < CLOSED_MAX) t->nclosed++;
    }
    if (t->ntabs == 1) {                                         /* the last one: it starts over at the home page */
        tab_free(&t->tab);
        memset(&t->tab, 0, sizeof(tab_t));
        t->hpos = -1;
        go(a, t, HOME, 1);
        return;
    }
    if (i == t->cur) {
        tab_free(&t->tab);
        int j = i < t->ntabs - 1 ? i + 1 : i - 1;
        tab_load(a, t, j);
    } else {
        tab_free(t->saved[i]);
        free(t->saved[i]);
        t->saved[i] = 0;
    }
    for (int k = i; k < t->ntabs - 1; k++) t->saved[k] = t->saved[k + 1];
    t->saved[t->ntabs - 1] = 0;
    t->ntabs--;
    if (t->cur > i) t->cur--;
    a->dirty = 1;
}

static void tab_reopen(uiapp_t *a, tf_t *t)
{
    if (!t->nclosed) { toast(t, "No closed tabs"); return; }
    char u[URL_MAX];
    strcpy(u, t->closed[0]);
    for (int k = 0; k < CLOSED_MAX - 1; k++) strcpy(t->closed[k], t->closed[k + 1]);
    t->nclosed--;
    tab_new(a, t, u, 1);
}

static const char *tab_title(tf_t *t, int i)
{
    const char *ti = i == t->cur ? t->title : t->saved[i]->title;
    const char *u = i == t->cur ? t->url : t->saved[i]->url;
    return ti[0] ? ti : u[0] ? u : "New tab";
}

static int tab_w(tf_t *t, int w)
{
    int tw = (w - 48) / t->ntabs;
    return tw > 210 ? 210 : tw < 70 ? 70 : tw;
}

static void draw_tabs(gfx_surface_t *s, tf_t *t, int x, int y, int w)
{
    gfx_fill(s, x, y, w, TAB_H, UI_FIELD);
    int tw = tab_w(t, w);
    for (int i = 0; i < t->ntabs; i++) {
        int tx = x + 6 + i * tw, on = i == t->cur;
        if (on) {
            gfx_rrect(s, tx, y + 5, tw - 4, TAB_H - 5, 8, UI_TOOLBAR, 255);
            gfx_fill(s, tx, y + TAB_H - 8, tw - 4, 8, UI_TOOLBAR);
        } else if (i + 1 < t->ntabs && i + 1 != t->cur) gfx_fill(s, tx + tw - 5, y + 10, 1, TAB_H - 18, UI_LINE);
        ui_text_fit(s, &ui_font_small, tx + 10, y + 10, tw - 4 - 10 - 26, tab_title(t, i), on ? UI_TEXT : UI_DIM);
        gfx_text_center(s, &ui_font_regular, tx + tw - 4 - 24, y + 7, 20, "\xC3\x97", on ? UI_DIM : UI_LINE, 255);
    }
    gfx_text_center(s, &ui_font_title, x + 6 + t->ntabs * tw, y + 2, 28, "+", UI_DIM, 255);
}

/* ------------------------------------------------------------------ the address bar */

static int prev_char(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int next_char(const char *s, int i)
{
    int l = (int)strlen(s);
    if (i >= l) return l;
    i++;
    while (i < l && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void field_insert(tf_t *t, int cp)
{
    char u[4];
    int n = utf8_enc(cp, u);
    if (t->select_all) { t->field[0] = 0; t->caret = 0; t->select_all = 0; }
    int l = (int)strlen(t->field);
    if (l + n >= URL_MAX) return;
    memmove(t->field + t->caret + n, t->field + t->caret, (size_t)(l - t->caret + 1));
    memcpy(t->field + t->caret, u, (size_t)n);
    t->caret += n;
}

static void field_delete(tf_t *t, int forward)
{
    if (t->select_all) { t->field[0] = 0; t->caret = 0; t->select_all = 0; return; }
    int l = (int)strlen(t->field);
    int from = forward ? t->caret : prev_char(t->field, t->caret);
    int to = forward ? next_char(t->field, t->caret) : t->caret;
    if (from == to) return;
    memmove(t->field + from, t->field + to, (size_t)(l - to + 1));
    t->caret = from;
}

/* where the field's text starts on screen: it scrolls so that the caret stays in view */
static int field_text_x(tf_t *t, int fx, int fw)
{
    char tmp[URL_MAX];
    memcpy(tmp, t->field, (size_t)t->caret);
    tmp[t->caret] = 0;
    int cw = gfx_text_width(&ui_font_regular, tmp), avail = fw - 20 - STAR_W, off = 0;
    if (t->editing && cw > avail - 14) off = cw - (avail - 14);
    return fx + 12 - off;
}

static int caret_at(tf_t *t, int px, int fx, int fw)
{
    int tx = field_text_x(t, fx, fw), best = 0, l = (int)strlen(t->field);
    char tmp[URL_MAX];
    for (int i = 0; i <= l; i = i < l ? next_char(t->field, i) : l + 1) {
        memcpy(tmp, t->field, (size_t)i);
        tmp[i] = 0;
        if (tx + gfx_text_width(&ui_font_regular, tmp) <= px + 3) best = i;
    }
    return best;
}

/* what the address bar's text could stand for: bookmarks and pages visited */
static void sug_add(tf_t *t, const char *url, const char *title)
{
    if (t->sug_n >= SUG_MAX || strlen(url) >= sizeof(t->sug_url[0])) return;
    for (int i = 0; i < t->sug_n; i++) if (!strcmp(t->sug_url[i], url)) return;
    strcpy(t->sug_url[t->sug_n], url);
    strncpy(t->sug_title[t->sug_n], title, sizeof(t->sug_title[0]) - 1);
    t->sug_title[t->sug_n][sizeof(t->sug_title[0]) - 1] = 0;
    t->sug_n++;
}

static const char *url_core(const char *u)
{
    if (starts(u, "https://")) u += 8;
    else if (starts(u, "http://")) u += 7;
    if (starts(u, "www.")) u += 4;
    return u;
}

static void sug_update(tf_t *t)
{
    t->sug_n = 0;
    t->sug_sel = -1;
    if (!t->editing || t->select_all || !t->field[0]) return;
    store_init();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nbm; i++)
            if (pass == 0 ? ci_prefix(url_core(bm[i].url), t->field) : (ci_find(bm[i].url, t->field) || ci_find(bm[i].title, t->field)))
                sug_add(t, bm[i].url, bm[i].title);
        for (int i = 0; i < nhs; i++)
            if (pass == 0 ? ci_prefix(url_core(hs[i].url), t->field) : (ci_find(hs[i].url, t->field) || ci_find(hs[i].title, t->field)))
                sug_add(t, hs[i].url, hs[i].title);
    }
}

static void bookmark_toggle(tf_t *t)
{
    if (!t->url[0] || starts(t->url, "tfire:")) { toast(t, "This page can't be bookmarked"); return; }
    int r = bm_toggle(t->url, t->title);
    toast(t, r == 1 ? "Bookmark added" : r == 0 ? "Bookmark removed" : "Can't bookmark this page");
}

/* ------------------------------------------------------------------ find in page */

#define FIND_MAXM 2048
#define FIND_MAXCHARS (1 << 20)
static char *fbuf;                 /* the page's words in a row, lower case, one space between them */
static int *fown;                  /* the item each character of fbuf belongs to */
static size_t fcap;
static uint8_t *fhit;              /* per item: 1 a match, 2 the match shown now */
static int fhit_n;
static int mfirst[FIND_MAXM], mlast[FIND_MAXM];
static const tf_t *find_owner;

static void find_scan(tf_t *t, tf_layout *L)
{
    t->find_n = 0;
    find_owner = t;
    if (L->n > fhit_n) {
        uint8_t *nh = realloc(fhit, (size_t)L->n);
        if (!nh) return;
        fhit = nh;
        fhit_n = L->n;
    }
    memset(fhit, 0, (size_t)fhit_n);
    if (!t->find_len) return;
    size_t total = 0;
    for (int i = 0; i < L->n; i++) if (L->items[i].kind == IT_TEXT) total += (size_t)L->items[i].len + 1;
    if (total > FIND_MAXCHARS) total = FIND_MAXCHARS;
    if (total + 1 > fcap) {
        char *nb = realloc(fbuf, total + 1);
        int *no = realloc(fown, (total + 1) * sizeof(int));
        if (nb) fbuf = nb;
        if (no) fown = no;
        if (!nb || !no) return;
        fcap = total + 1;
    }
    size_t pos = 0;
    for (int i = 0; i < L->n; i++) {
        tf_item *it = &L->items[i];
        if (it->kind != IT_TEXT) continue;
        if (pos + it->len + 1 > fcap) break;
        for (int k = 0; k < it->len; k++) { fbuf[pos + k] = (char)lower_((unsigned char)L->pool[it->text + k]); fown[pos + k] = i; }
        fbuf[pos + it->len] = ' ';
        fown[pos + it->len] = i;
        pos += (size_t)it->len + 1;
    }
    char q[64];
    int ql = t->find_len;
    for (int i = 0; i < ql; i++) q[i] = (char)lower_((unsigned char)t->find_q[i]);
    for (size_t o = 0; o + (size_t)ql <= pos && t->find_n < FIND_MAXM;) {
        if (fbuf[o] == q[0] && !memcmp(fbuf + o, q, (size_t)ql)) {
            mfirst[t->find_n] = fown[o];
            mlast[t->find_n] = fown[o + (size_t)ql - 1];
            for (int k = 0; k < ql; k++) fhit[fown[o + (size_t)k]] = 1;
            t->find_n++;
            o += (size_t)ql;
        } else o++;
    }
    if (t->find_i >= t->find_n) t->find_i = 0;
    if (t->find_n)
        for (int i = mfirst[t->find_i]; i <= mlast[t->find_i] && i < L->n; i++)
            if (L->items[i].kind == IT_TEXT) fhit[i] = 2;
}

/* scans again; scroll = show the match that is current */
static void find_run(tf_t *t, int scroll)
{
    tf_page *pg = t->prev ? t->prev : t->page;
    if (!pg) return;
    tf_layout *L = page_layout(pg);
    find_scan(t, L);
    t->find_last_n = L->n;
    t->find_dirty = 0;
    if (scroll && t->find_n) {
        t->scroll = L->items[mfirst[t->find_i]].y - t->view_h / 3;
        if (t->scroll < 0) t->scroll = 0;
    }
}

static void find_step(tf_t *t, int d)
{
    if (!t->find_n) return;
    t->find_i = (t->find_i + d + t->find_n) % t->find_n;
    find_run(t, 1);
}

static void find_open(tf_t *t)
{
    t->find_on = 1;
    t->find_focus = 1;
    t->find_dirty = 1;
}

static void find_close(tf_t *t)
{
    t->find_on = 0;
    t->find_focus = 0;
}

/* ------------------------------------------------------------------ drawing */

#define BTN 32
static int back_x(void) { return 10; }
static int fwd_x(void) { return 10 + BTN + 4; }
static int reload_x(void) { return 10 + 2 * (BTN + 4); }
static int home_x(void) { return 10 + 3 * (BTN + 4); }
static int field_x(void) { return 10 + 4 * (BTN + 4) + 6; }

/* ------------------------------------------------------------------ downloads */

static void size_text(uint32_t n, char *out)
{
    const char *unit = "bytes";
    uint32_t whole = n, tenth = 0;
    if (n >= 1024 * 1024) { whole = n / (1024 * 1024); tenth = n % (1024 * 1024) * 10 / (1024 * 1024); unit = "MB"; }
    else if (n >= 1024) { whole = n / 1024; tenth = n % 1024 * 10 / 1024; unit = "KB"; }
    char num[16];
    int k = 0;
    char r[12];
    int rl = 0;
    do { r[rl++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (rl) num[k++] = r[--rl];
    if (unit[0] != 'b') { num[k++] = '.'; num[k++] = (char)('0' + tenth); }
    num[k] = 0;
    strcpy(out, num);
    strcat(out, " ");
    strcat(out, unit);
}

/* the file saved in ~/Downloads (as "name (2).ext" when name is taken), told in the bar and a notification */
static void save_download(tf_t *t, const char *name, const uint8_t *data, uint32_t len)
{
    strcpy(t->dl_dir, uihost_home());
    if (strcmp(t->dl_dir, "/")) strcat(t->dl_dir, "/");
    strcat(t->dl_dir, "Downloads");
    uihost_mkdir(t->dl_dir);
    static uihost_entry_t have[256];
    int n = uihost_list(t->dl_dir, have, 256);
    char fname[128];
    strcpy(fname, name);
    for (int k = 2; k < 100; k++) {
        int taken = 0;
        for (int i = 0; i < n && !taken; i++) taken = !strcmp(have[i].name, fname);
        if (!taken) break;
        const char *dot = 0;
        for (const char *c = name; *c; c++) if (*c == '.') dot = c;
        int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
        if (base > 100) base = 100;
        memcpy(fname, name, (size_t)base);
        fname[base] = 0;
        char num[8] = " (", d[4];
        int dl = 0, v = k;
        while (v) { d[dl++] = (char)('0' + v % 10); v /= 10; }
        int o = 2;
        while (dl) num[o++] = d[--dl];
        num[o++] = ')';
        num[o] = 0;
        strcat(fname, num);
        if (dot && dot != name) strcat(fname, dot);
    }
    char path[340];
    strcpy(path, t->dl_dir);
    strcat(path, "/");
    strcat(path, fname);
    t->dl_ok = uihost_write(path, data, len) == 0;
    char size[24];
    size_text(len, size);
    strcpy(t->dl_text, t->dl_ok ? "Downloaded " : "Could not save ");
    strncat_(t->dl_text, fname, sizeof(t->dl_text));
    if (t->dl_ok) {
        strncat_(t->dl_text, "  \xE2\x80\x94  ", sizeof(t->dl_text));
        strncat_(t->dl_text, size, sizeof(t->dl_text));
        strncat_(t->dl_text, ", in Downloads", sizeof(t->dl_text));
    }
    uihost_notify("tfire", "tfire", t->dl_ok ? "Download finished" : "Download failed", fname, 0, 0);
}

static void arrow(gfx_surface_t *s, int cx, int cy, int dir, uint32_t c)
{
    gfx_capsule(s, cx - 8 * dir, cy, cx + 8 * dir, cy, 6, c, 255);       /* dir -1 points left, 1 right */
    gfx_capsule(s, cx + 8 * dir, cy, cx + 2 * dir, cy - 6, 6, c, 255);
    gfx_capsule(s, cx + 8 * dir, cy, cx + 2 * dir, cy + 6, 6, c, 255);
}

static void reload_icon(gfx_surface_t *s, int cx, int cy, uint32_t c)
{
    /* three quarters of a circle, the open end at the top right with its arrow head */
    for (int d = 60; d < 360; d += 12) {
        int x0 = cx + gfx_cos(d) * 8 / 4096, y0 = cy - gfx_sin(d) * 8 / 4096;
        int x1 = cx + gfx_cos(d + 12) * 8 / 4096, y1 = cy - gfx_sin(d + 12) * 8 / 4096;
        gfx_capsule(s, x0, y0, x1, y1, 6, c, 255);
    }
    int ax = cx + gfx_cos(60) * 8 / 4096, ay = cy - gfx_sin(60) * 8 / 4096;
    gfx_capsule(s, ax, ay, ax - 6, ay - 2, 6, c, 255);
    gfx_capsule(s, ax, ay, ax + 1, ay + 6, 6, c, 255);
}

static void home_icon(gfx_surface_t *s, int cx, int cy, uint32_t c)
{
    gfx_capsule(s, cx - 8, cy - 1, cx, cy - 8, 5, c, 255);
    gfx_capsule(s, cx, cy - 8, cx + 8, cy - 1, 5, c, 255);
    gfx_capsule(s, cx - 5, cy - 3, cx - 5, cy + 7, 5, c, 255);
    gfx_capsule(s, cx + 5, cy - 3, cx + 5, cy + 7, 5, c, 255);
    gfx_capsule(s, cx - 5, cy + 7, cx + 5, cy + 7, 5, c, 255);
}

#include "tfire_paint.h"

/* a word of the page: its text from the layout's pool, 0-terminated */
static void item_text(tf_layout *L, tf_item *it, char *out, int cap)
{
    int n = it->len < cap - 1 ? it->len : cap - 1;
    memcpy(out, L->pool + it->text, (size_t)n);
    out[n] = 0;
}

static void draw_field(gfx_surface_t *c, tf_t *t, tf_layout *L, tf_item *it, int x, int y)
{
    char label[256];
    item_text(L, it, label, sizeof(label));
    int kind = (int)it->color;
    const ui_font_t *f = fonts[it->font < tfire_font_count ? it->font : 0];
    int focused = shown(t) && it->node && page_focus(shown(t)) == it->node;
    if (kind == 2) {                                         /* a check box or radio button */
        const char *type = dom_attr(it->node, "type");
        int radio = type && !strcmp(type, "radio");
        gfx_rrect(c, x, y, it->w, it->h, radio ? it->w / 2 : 3, label[0] ? UI_ACCENT : 0xFFFFFF, 255);
        gfx_rrect_outline(c, x, y, it->w, it->h, radio ? it->w / 2 : 3, label[0] ? UI_ACCENT : 0x8E8E93, 255);
        if (label[0]) {
            if (radio) gfx_rrect(c, x + it->w / 2 - 2, y + it->h / 2 - 2, 5, 5, 2, 0xFFFFFF, 255);
            else { gfx_capsule(c, x + 3, y + 7, x + 5, y + 10, 3, 0xFFFFFF, 255); gfx_capsule(c, x + 5, y + 10, x + 10, y + 3, 3, 0xFFFFFF, 255); }
        }
        return;
    }
    uint32_t bg = kind == 1 ? 0xE5E5EA : 0xFFFFFF;
    gfx_rrect(c, x, y, it->w, it->h, 4, bg, 255);
    gfx_rrect_outline(c, x, y, it->w, it->h, 4, focused ? UI_ACCENT : 0x8E8E93, 255);
    if (focused) gfx_rrect_outline(c, x - 1, y - 1, it->w + 2, it->h + 2, 5, UI_ACCENT, 120);
    gfx_surface_t in = *c;
    gfx_clip(&in, x + 3, y, it->w - 6, it->h);
    int ty = y + (it->h - (f->ascent + f->descent)) / 2;
    if (kind == 1) {
        gfx_text_center(&in, f, x, ty, it->w, label, 0x111827, 255);
    } else {
        const char *ph = !label[0] && it->node ? dom_attr(it->node, "placeholder") : 0;
        int tw = gfx_text_width(f, label);
        int tx = x + 5;
        if (focused && tw > it->w - 14) tx = x + it->w - 9 - tw;            /* the end, where the caret is, in view */
        gfx_text(&in, f, tx, ty, ph ? ph : label, ph ? 0x9CA3AF : 0x111827, 255);
        if (kind == 3) arrow(&in, x + it->w - 10, y + it->h / 2, 1, 0x6B7280);
        if (focused && page_caret_on(shown(t))) gfx_fill(&in, tx + (ph ? 0 : tw) + 1, ty, 2, f->ascent + f->descent, 0x111827);
    }
}

/* a star for the address bar: an outline, or filled (the page is bookmarked) */
static void star_icon(gfx_surface_t *s, int cx, int cy, int on)
{
    static const int vx[5] = { 0, 8, 5, -5, -8 }, vy[5] = { -8, -2, 6, 6, -2 };
    static const int ord[6] = { 0, 2, 4, 1, 3, 0 };
    uint32_t c = on ? 0xF5B301 : UI_DIM;
    if (on) for (int i = 0; i < 5; i++) gfx_capsule(s, cx, cy, cx + vx[i] * 3 / 4, cy + vy[i] * 3 / 4, 10, c, 255);
    for (int i = 0; i < 5; i++) gfx_capsule(s, cx + vx[ord[i]], cy + vy[ord[i]], cx + vx[ord[i + 1]], cy + vy[ord[i + 1]], on ? 5 : 3, c, 255);
}

/* the find bar: its parts, from the left edge */
#define FIND_FX  60
#define FIND_FW  260
#define FIND_PREV_X 440
#define FIND_NEXT_X 510
#define FIND_BTN_W  64

static void draw_find(gfx_surface_t *s, tf_t *t, int x, int y, int w)
{
    gfx_fill(s, x, y, w, FIND_H, UI_TOOLBAR);
    gfx_hline(s, x, y, w, UI_LINE, 255);
    gfx_text(s, &ui_font_regular, x + 14, y + 10, "Find", UI_DIM, 255);
    gfx_rrect(s, x + FIND_FX, y + 5, FIND_FW, 28, 7, UI_FIELD, 255);
    if (t->find_focus && !t->editing) gfx_rrect_outline(s, x + FIND_FX - 1, y + 4, FIND_FW + 2, 30, 8, UI_ACCENT, 255);
    gfx_surface_t c = *s;
    gfx_clip(&c, x + FIND_FX + 8, y + 5, FIND_FW - 16, 28);
    int tw = gfx_text_width(&ui_font_regular, t->find_q);
    int tx = x + FIND_FX + 10 - (tw > FIND_FW - 30 ? tw - (FIND_FW - 30) : 0);
    gfx_text(&c, &ui_font_regular, tx, y + 10, t->find_q, UI_TEXT, 255);
    if (t->find_focus && !t->editing && (uihost_ms() / 500) % 2 == 0) gfx_fill(&c, tx + tw + 1, y + 10, 2, 18, UI_TEXT);
    char info[40] = "";
    if (t->find_len) {
        if (!t->find_n) strcpy(info, "No matches");
        else {
            sb_t b = { 0 };
            sb_int(&b, t->find_i + 1);
            sb_add(&b, " of ");
            sb_int(&b, t->find_n);
            if (t->find_n >= FIND_MAXM) sb_add(&b, "+");
            if (b.p && b.len < (int)sizeof(info)) strcpy(info, b.p);
            free(b.p);
        }
    }
    gfx_text(s, &ui_font_small, x + FIND_FX + FIND_FW + 12, y + 12, info, UI_DIM, 255);
    ui_button(s, x + FIND_PREV_X, y + 5, FIND_BTN_W, 28, "Prev", UI_FIELD, UI_TEXT, 0);
    ui_button(s, x + FIND_NEXT_X, y + 5, FIND_BTN_W, 28, "Next", UI_FIELD, UI_TEXT, 0);
    gfx_text_center(s, &ui_font_title, x + w - 40, y + 4, 28, "\xC3\x97", UI_DIM, 255);
}

/* the suggestions under the address bar */
static void draw_suggestions(gfx_surface_t *s, tf_t *t, int x, int y, int w)
{
    int fx = x + field_x(), fw = w - field_x() - 12, h = t->sug_n * SUG_ROW + 8;
    gfx_shadow(s, fx, y + TB_H, fw, h, 40);
    gfx_rrect(s, fx, y + TB_H - 2, fw, h, 8, UI_TOOLBAR, 255);
    gfx_rrect_outline(s, fx, y + TB_H - 2, fw, h, 8, UI_LINE, 255);
    for (int i = 0; i < t->sug_n; i++) {
        int ry = y + TB_H + 2 + i * SUG_ROW;
        if (i == t->sug_sel) gfx_rrect(s, fx + 4, ry, fw - 8, SUG_ROW - 2, 6, UI_ACCENT, 70);
        ui_text_fit(s, &ui_font_regular, fx + 14, ry + 3, fw - 28, t->sug_title[i], UI_TEXT);
        ui_text_fit(s, &ui_font_small, fx + 14, ry + 24, fw - 28, t->sug_url[i], UI_DIM);
    }
}

static void tf_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    tf_t *t = a->st;
    int oy = tabs_h(t);
    if (oy) { draw_tabs(s, t, x, y, w); y += oy; h -= oy; }
    /* toolbar */
    gfx_fill(s, x, y, w, TB_H, UI_TOOLBAR);
    gfx_hline(s, x, y + TB_H - 1, w, UI_LINE, 255);
    uint32_t on = UI_TEXT, off = UI_DIM;
    arrow(s, x + back_x() + BTN / 2, y + TB_H / 2, -1, t->hpos > 0 ? on : off);
    arrow(s, x + fwd_x() + BTN / 2, y + TB_H / 2, 1, t->hpos < t->nhist - 1 ? on : off);
    reload_icon(s, x + reload_x() + BTN / 2, y + TB_H / 2, on);
    home_icon(s, x + home_x() + BTN / 2, y + TB_H / 2, on);
    int fx = x + field_x(), fw = w - field_x() - 12;
    gfx_rrect(s, fx, y + 8, fw, 30, 8, UI_FIELD, 255);
    if (t->editing) gfx_rrect_outline(s, fx - 1, y + 7, fw + 2, 32, 9, UI_ACCENT, 255);
    {
        gfx_surface_t c = *s;
        gfx_clip(&c, fx + 10, y + 8, fw - 20 - STAR_W, 30);
        int tw = gfx_text_width(&ui_font_regular, t->field);
        int tx = field_text_x(t, fx, fw);
        if (t->editing && t->select_all && t->field[0]) gfx_fill_alpha(&c, tx - 2, y + 13, tw + 4, 20, UI_ACCENT, 70);
        gfx_text(&c, &ui_font_regular, tx, y + 14, t->field, t->editing ? UI_TEXT : UI_DIM, 255);
        if (t->editing && !t->select_all && (uihost_ms() / 500) % 2 == 0) {
            char tmp[URL_MAX];
            memcpy(tmp, t->field, (size_t)t->caret);
            tmp[t->caret] = 0;
            gfx_fill(&c, tx + gfx_text_width(&ui_font_regular, tmp) + 1, y + 14, 2, 18, UI_TEXT);
        }
    }
    if (!starts(t->url, "tfire:")) star_icon(s, fx + fw - STAR_W / 2 - 2, y + TB_H / 2, bm_find(t->url) >= 0);
    /* loading: a bar sweeping along the bottom of the toolbar */
    if (t->page && page_loading(t->page)) {
        int p = (int)((uihost_ms() - t->load_start) / 4 % (uint32_t)(w + 160)) - 160;
        gfx_surface_t c = *s;
        gfx_clip(&c, x, y + TB_H - 3, w, 3);
        gfx_fill(&c, x + p, y + TB_H - 3, 160, 3, UI_ACCENT);
    }

    /* the page */
    int findh = t->find_on ? FIND_H : 0;
    int py = y + TB_H, ph = h - TB_H - findh;
    if (ph < 40) ph = 40;
    t->view_w = w;
    t->view_h = ph;
    tf_page *pg = shown(t);
    tf_layout *L = pg ? page_layout(pg) : 0;
    gfx_fill(s, x, py, w, ph, L && L->n ? L->page_bg : 0xFFFFFF);
    if (L) {
        if (t->find_on && (t->find_dirty || L->n != t->find_last_n || find_owner != t)) find_run(t, 0);
        int doc_h = L->doc_h + 16;
        int maxs = doc_h - ph;
        if (maxs < 0) maxs = 0;
        if (t->scroll > maxs) t->scroll = maxs;
        if (t->scroll < 0) t->scroll = 0;
        gfx_surface_t c = *s;
        gfx_clip(&c, x, py, w, ph);
        for (int i = 0; i < L->n; i++) {
            tf_item *it = &L->items[i];
            int ix = x + it->x, iy = py + it->y - t->scroll;
            int reach = it->rot ? (it->w > it->h ? it->w : it->h) : it->kind == IT_SHADOW ? it->deco : 0;   /* (turned: its corners reach further) */
            if (iy - reach > py + ph || iy + it->h + reach < py) continue;
            switch (it->kind) {
            case IT_RECT: case IT_TEXT: case IT_IMAGE: case IT_SHADOW: case IT_GRAD: case IT_FRAME:
                tfp_item(&c, pg, L, it, ix, iy, fonts);
                break;
            case IT_FIELD:
                draw_field(&c, t, L, it, ix, iy);
                break;
            default:
                break;
            }
            if (t->find_on && it->kind == IT_TEXT && find_owner == t && i < fhit_n && fhit[i])
                gfx_fill_alpha(&c, ix - 1, iy, it->w + 2, it->h, fhit[i] == 2 ? 0xFF8A1E : 0xFFE933, fhit[i] == 2 ? 150 : 110);
        }
        /* scroll bar */
        if (maxs > 0) {
            int bh = ph * ph / doc_h;
            if (bh < 30) bh = 30;
            int bt = py + (ph - bh) * t->scroll / maxs;
            gfx_rrect(s, x + w - 8, bt + 2, 5, bh - 4, 2, 0x000000, 70);
        }
    }
    /* the last download: a bar along the top of the page (the bottom can be under the Dock) */
    if (t->dl_text[0]) {
        int by = py;
        gfx_fill(s, x, by, w, DL_H, 0xF3F4F6);
        gfx_hline(s, x, by + DL_H - 1, w, 0xD1D5DB, 255);
        int cx = x + 22, cy = by + DL_H / 2;                  /* an arrow down onto a line */
        uint32_t ic = t->dl_ok ? UI_ACCENT : 0xDC2626;
        gfx_capsule(s, cx, cy - 8, cx, cy + 3, 6, ic, 255);
        gfx_capsule(s, cx, cy + 3, cx - 5, cy - 2, 6, ic, 255);
        gfx_capsule(s, cx, cy + 3, cx + 5, cy - 2, 6, ic, 255);
        gfx_capsule(s, cx - 7, cy + 8, cx + 7, cy + 8, 6, ic, 255);
        gfx_surface_t ct = *s;
        gfx_clip(&ct, x + 40, by, w - 40 - 190, DL_H);
        gfx_text(&ct, &ui_font_regular, x + 40, by + (DL_H - 18) / 2, t->dl_text, 0x111827, 255);
        if (t->dl_ok) ui_button(s, x + w - DL_SHOW_X, by + 6, 130, DL_H - 12, "Show in Files", 0xFFFFFF, 0x111827, 0);
        gfx_text_center(s, &ui_font_title, x + w - DL_CLOSE_X, by + 2, 28, "\xC3\x97", 0x6B7280, 255);
    }
    /* the link under the pointer, at the bottom left (as other browsers show it) */
    if (t->status[0]) {
        int sw = gfx_text_width(&ui_font_small, t->status) + 16;
        if (sw > w - 20) sw = w - 20;
        int sy = py + ph;
        gfx_surface_t c2 = *s;
        gfx_clip(&c2, x, sy - 24, sw, 24);
        gfx_rrect(&c2, x, sy - 22, sw + 6, 22, 6, 0xF3F4F6, 255);
        gfx_rrect_outline(&c2, x, sy - 22, sw + 6, 22, 6, 0xD1D5DB, 255);
        gfx_text(&c2, &ui_font_small, x + 8, sy - 18, t->status, 0x374151, 255);
    }
    if (t->find_on) draw_find(s, t, x, py + ph, w);
    /* a short message, over the middle of the bottom of the page */
    if (t->toast[0]) {
        int tw = gfx_text_width(&ui_font_regular, t->toast) + 36;
        int tx = x + (w - tw) / 2, ty = py + ph - 64;
        gfx_rrect(s, tx, ty, tw, 36, 18, 0x1F2937, 235);
        gfx_text(s, &ui_font_regular, tx + 18, ty + 9, t->toast, 0xFFFFFF, 255);
    }
    if (t->editing && t->sug_n) draw_suggestions(s, t, x, y, w);
}

static void tf_press(uiapp_t *a, int px, int py, int button)
{
    tf_t *t = a->st;
    a->dirty = 1;
    int oy = tabs_h(t);
    if (oy && py < oy) {                                         /* the tab strip */
        int tw = tab_w(t, t->view_w), i = (px - 6) / tw;
        if (px >= 6 + t->ntabs * tw) { if (px < 6 + t->ntabs * tw + 28) tab_new(a, t, HOME, 1); return; }
        if (px < 6 || i < 0 || i >= t->ntabs) return;
        if (button == 3 || px >= 6 + i * tw + tw - 4 - 26) tab_close(a, t, i);
        else tab_switch(a, t, i);
        return;
    }
    py -= oy;
    if (py < TB_H) {
        int fx = field_x(), fw = t->view_w - field_x() - 12;
        if (ui_hit(px, py, back_x(), 6, BTN, 34)) { if (t->hpos > 0) { t->hpos--; go(a, t, t->hist[t->hpos], 0); } return; }
        if (ui_hit(px, py, fwd_x(), 6, BTN, 34)) { if (t->hpos < t->nhist - 1) { t->hpos++; go(a, t, t->hist[t->hpos], 0); } return; }
        if (ui_hit(px, py, reload_x(), 6, BTN, 34)) { go(a, t, t->url, 0); return; }
        if (ui_hit(px, py, home_x(), 6, BTN, 34)) { go(a, t, HOME, 1); return; }
        if (!starts(t->url, "tfire:") && ui_hit(px, py, fx + fw - STAR_W, 8, STAR_W, 30)) { bookmark_toggle(t); return; }
        if (px >= fx) {
            if (!t->editing) { t->editing = 1; t->select_all = 1; field_reset(t); }
            else { t->select_all = 0; t->caret = caret_at(t, px, fx, fw); }
        }
        return;
    }
    if (t->editing && t->sug_n) {                                /* a suggestion */
        int fx = field_x(), fw = t->view_w - field_x() - 12, i = (py - TB_H - 2) / SUG_ROW;
        if (px >= fx && px < fx + fw && py >= TB_H && i >= 0 && i < t->sug_n) {
            char u[256];
            strcpy(u, t->sug_url[i]);
            go(a, t, u, 1);
            return;
        }
    }
    if (t->editing) { t->editing = 0; field_reset(t); }
    if (t->find_on && py >= TB_H + t->view_h) {                  /* the find bar */
        int x = px;
        if (x >= t->view_w - 48) find_close(t);
        else if (x >= FIND_PREV_X && x < FIND_PREV_X + FIND_BTN_W) find_step(t, -1);
        else if (x >= FIND_NEXT_X && x < FIND_NEXT_X + FIND_BTN_W) find_step(t, 1);
        else if (x >= FIND_FX && x < FIND_FX + FIND_FW) t->find_focus = 1;
        return;
    }
    if (t->find_on) t->find_focus = 0;
    if (t->dl_text[0] && py < TB_H + DL_H) {                       /* the download bar */
        if (t->dl_ok && px >= t->view_w - DL_SHOW_X && px < t->view_w - DL_SHOW_X + 130) uihost_open("files", t->dl_dir);
        else if (px >= t->view_w - DL_CLOSE_X) t->dl_text[0] = 0;
        return;
    }
    if (!shown(t)) return;
    if (button == 3) {                                           /* the middle button: a link opens in a new tab */
        const char *href = page_link_at(shown(t), px, py - TB_H + t->scroll);
        char u[URL_MAX];
        if (href && strncmp(href, "javascript:", 11) && href[0] != '#' && page_resolve(page_url(shown(t)), href, u, sizeof(u)) >= 0 && u[0])
            tab_new(a, t, u, 0);
        return;
    }
    page_click(shown(t), px, py - TB_H + t->scroll);
}

/* what the desktop hands over is a character: Ctrl+T arrives as 0x14 (which is also the right arrow's number), Ctrl+Tab as 9 and
 * Shift+Enter as a plain Enter. With Ctrl held, a control character is the letter's shortcut; Shift is asked of the keyboard. */
static int tos_key(int k)
{
    int held = keyboard_last_mods();                       /* (not uihost_keys_held: Ctrl may be let go before the key is looked at) */
    if (k == '\n' && (held & 1)) return TF_KEY_SENTER;
    if ((held & 2) && k >= '1' && k <= '9') return TF_KEY_TAB1 + (k - '1');
    if (!(held & 2) || k < 1 || k > 26) return k;
    switch (k + 'a' - 1) {
    case 'l': return TF_KEY_URL;
    case 'r': return TF_KEY_RELOAD;
    case 't': return (held & 1) ? TF_KEY_REOPEN : TF_KEY_NEWTAB;
    case 'w': return TF_KEY_CLOSETAB;
    case 'i': return (held & 1) ? TF_KEY_PREVTAB : TF_KEY_NEXTTAB;     /* Ctrl+Tab */
    case 'f': return TF_KEY_FIND;
    case 'd': return TF_KEY_BOOKMARK;
    case 'h': return TF_KEY_HISTORY;
    case 'o': return (held & 1) ? TF_KEY_BOOKMARKS : k;
    case 'a': return TF_KEY_SELALL;
    case 'c': return TF_KEY_COPY;
    case 'x': return TF_KEY_CUT;
    case 'v': return TF_KEY_PASTE;
    }
    return k;
}

static void tf_key(uiapp_t *a, int k)
{
    tf_t *t = a->st;
    a->dirty = 1;
    k = tos_key(k);
    switch (k) {
    case TF_KEY_URL: t->editing = 1; t->select_all = 1; field_reset(t); return;
    case TF_KEY_RELOAD: if (t->url[0]) go(a, t, t->url, 0); return;
    case TF_KEY_BACK: if (t->hpos > 0) { t->hpos--; go(a, t, t->hist[t->hpos], 0); } return;
    case TF_KEY_FORWARD: if (t->hpos < t->nhist - 1) { t->hpos++; go(a, t, t->hist[t->hpos], 0); } return;
    case TF_KEY_NEWTAB: tab_new(a, t, HOME, 1); t->editing = 1; t->select_all = 1; field_reset(t); return;
    case TF_KEY_CLOSETAB: tab_close(a, t, t->cur); return;
    case TF_KEY_NEXTTAB: tab_switch(a, t, (t->cur + 1) % t->ntabs); return;
    case TF_KEY_PREVTAB: tab_switch(a, t, (t->cur + t->ntabs - 1) % t->ntabs); return;
    case TF_KEY_REOPEN: tab_reopen(a, t); return;
    case TF_KEY_FIND: find_open(t); t->editing = 0; field_reset(t); return;
    case TF_KEY_BOOKMARK: bookmark_toggle(t); return;
    case TF_KEY_HISTORY: go(a, t, "tfire:history", 1); return;
    case TF_KEY_BOOKMARKS: go(a, t, "tfire:bookmarks", 1); return;
    case TF_KEY_SELALL: if (t->editing) t->select_all = 1; return;
    case TF_KEY_COPY: uihost_clipboard_set(t->editing ? t->field : t->url); if (!t->editing) toast(t, "Address copied"); return;
    case TF_KEY_PASTE:
        if (t->editing) { for (const char *p = tf_clip; *p; p++) if ((unsigned char)*p >= 32) field_insert(t, (unsigned char)*p); sug_update(t); }
        return;
    case TF_KEY_CUT:
        if (t->editing) { uihost_clipboard_set(t->field); t->field[0] = 0; t->caret = 0; t->select_all = 0; sug_update(t); }
        return;
    }
    if (k >= TF_KEY_TAB1 && k < TF_KEY_TAB1 + 9) {
        int i = k - TF_KEY_TAB1;
        tab_switch(a, t, i == 8 ? t->ntabs - 1 : i);
        return;
    }
    if (t->find_on && t->find_focus && !t->editing) {            /* typing in the find bar */
        if (k == '\n' || k == UI_KEY_DOWN) find_step(t, 1);
        else if (k == TF_KEY_SENTER || k == UI_KEY_UP) find_step(t, -1);
        else if (k == UI_KEY_ESC) find_close(t);
        else if (k == '\b' || k == UI_KEY_DEL) {
            if (t->find_len) { t->find_len = prev_char(t->find_q, t->find_len); t->find_q[t->find_len] = 0; t->find_i = 0; find_run(t, 1); }
        } else if (k >= 32 && k < 0x110000 && k != 127) {
            char u[4];
            int n = utf8_enc(k, u);
            if (t->find_len + n < (int)sizeof(t->find_q) - 1) {
                memcpy(t->find_q + t->find_len, u, (size_t)n);
                t->find_len += n;
                t->find_q[t->find_len] = 0;
                t->find_i = 0;
                find_run(t, 1);
            }
        } else if (k == TF_KEY_PGUP || k == TF_KEY_PGDN || k == TF_KEY_HOME || k == TF_KEY_END) { /* falls to the page below */ }
        if (!(k == TF_KEY_PGUP || k == TF_KEY_PGDN || k == TF_KEY_HOME || k == TF_KEY_END)) return;
    }
    if (t->editing) {
        if (k == TF_KEY_SENTER) k = '\n';
        if (k == '\n') {
            char url[URL_MAX];
            typed_to_url(t->field, url);
            go(a, t, url, 1);
        } else if (k == UI_KEY_ESC) {
            t->editing = 0;
            field_reset(t);
        } else if (k == '\b') { field_delete(t, 0); sug_update(t); }
        else if (k == UI_KEY_DEL) { field_delete(t, 1); sug_update(t); }
        else if (k == UI_KEY_LEFT) {
            if (t->select_all) { t->select_all = 0; t->caret = 0; }
            else t->caret = prev_char(t->field, t->caret);
        } else if (k == UI_KEY_RIGHT) {
            if (t->select_all) { t->select_all = 0; t->caret = (int)strlen(t->field); }
            else t->caret = next_char(t->field, t->caret);
        } else if (k == TF_KEY_HOME) { t->select_all = 0; t->caret = 0; }
        else if (k == TF_KEY_END) { t->select_all = 0; t->caret = (int)strlen(t->field); }
        else if (k == UI_KEY_DOWN && t->sug_n) {
            if (t->sug_sel < 0) strcpy(t->sug_typed, t->field);
            if (t->sug_sel < t->sug_n - 1) t->sug_sel++;
            strcpy(t->field, t->sug_url[t->sug_sel]);
            t->caret = (int)strlen(t->field);
            t->select_all = 0;
        } else if (k == UI_KEY_UP && t->sug_n && t->sug_sel >= 0) {
            t->sug_sel--;
            strcpy(t->field, t->sug_sel >= 0 ? t->sug_url[t->sug_sel] : t->sug_typed);
            t->caret = (int)strlen(t->field);
            t->select_all = 0;
        } else if (k >= 32 && k < 0x110000 && k != 127) { field_insert(t, k); sug_update(t); }
        return;
    }
    /* the page first (a form field, or a script that wants keys), then scrolling */
    if (k < 0x200000 && t->page && page_key(t->page, k)) return;
    int page = t->view_h - 40;
    if (k == UI_KEY_ESC && t->find_on) find_close(t);
    else if (k == UI_KEY_DOWN) t->scroll += 40;
    else if (k == UI_KEY_UP) t->scroll -= 40;
    else if (k == ' ' || k == TF_KEY_PGDN) t->scroll += page;
    else if (k == TF_KEY_PGUP) t->scroll -= page;
    else if (k == TF_KEY_HOME) t->scroll = 0;
    else if (k == TF_KEY_END) t->scroll = 1 << 24;               /* (the drawing keeps it inside the page) */
    else if (k == '\b' || k == UI_KEY_LEFT) { if (t->hpos > 0) { t->hpos--; go(a, t, t->hist[t->hpos], 0); } }
    else if (k == UI_KEY_RIGHT) { if (t->hpos < t->nhist - 1) { t->hpos++; go(a, t, t->hist[t->hpos], 0); } }
    else if (k == 'l' || k == '/') { t->editing = 1; t->select_all = 1; field_reset(t); }
}

static void tf_wheel(uiapp_t *a, int delta)
{
    tf_t *t = a->st;
    t->scroll += delta * 64;
    if (t->scroll < 0) t->scroll = 0;
    a->dirty = 1;
}

static int tf_tick(uiapp_t *a)
{
    tf_t *t = a->st;
    int redraw = 0;
    if (t->toast_until && uihost_ms() > t->toast_until) { t->toast_until = 0; t->toast[0] = 0; redraw = 1; }
    for (int i = 0; i < t->ntabs; i++) {                         /* the tabs behind keep loading */
        tab_t *s = i != t->cur ? t->saved[i] : 0;
        if (!s || !s->page) continue;
        page_tick(s->page, t->view_w, t->view_h);
        const char *pt = page_title(s->page);
        if (strncmp(s->title, pt, sizeof(s->title) - 1)) { strncpy(s->title, pt, sizeof(s->title) - 1); s->title[sizeof(s->title) - 1] = 0; redraw = 1; }
    }
    if (!t->page) return redraw;
    if (page_tick(t->page, t->view_w, t->view_h)) redraw = 1;
    {
        char name[128];
        void *raw;
        const uint8_t *data;
        uint32_t len;
        if (page_take_download(t->page, name, sizeof(name), &raw, &data, &len)) {
            save_download(t, name, data, len);
            free(raw);
            page_close(t->page);
            t->page = 0;
            if (t->prev) {                                   /* the page it was on stays */
                t->page = t->prev;
                t->prev = 0;
                strcpy(t->url, t->prev_url);
                field_reset(t);
                t->hpos = t->prev_hpos;
                t->nhist = t->prev_nhist;
            } else go(a, t, HOME, 0);
            set_title(a, t);
            return 1;
        }
        if (t->prev && page_fetched(t->page)) {              /* the new page's answer is here: it takes over */
            page_close(t->prev);
            t->prev = 0;
            t->scroll = 0;
            redraw = 1;
        }
    }
    tf_request nav;
    if (page_take_navigation(t->page, &nav)) {
        go_req(a, t, &nav, 1);
        tf_free(nav.body);
        return 1;
    }
    char u[URL_MAX];
    if (page_take_url_change(t->page, u, sizeof(u))) {
        strcpy(t->url, u);
        if (!t->editing) field_reset(t);
        if (t->hpos >= 0 && t->hpos < t->nhist) strcpy(t->hist[t->hpos], u);
        redraw = 1;
    }
    int sy = page_take_scroll(t->page);
    if (sy >= 0) { t->scroll = sy; redraw = 1; }
    page_scrolled(t->page, t->scroll);
    if (redraw) set_title(a, t);
    if (!t->recorded && !t->prev && page_fetched(t->page) && (page_title(t->page)[0] || !page_loading(t->page))) {
        t->recorded = 1;                                     /* a page that came: it is in the history */
        hs_record(page_url(t->page), page_title(t->page));
    }
    if (page_loading(t->page)) redraw = 1;                   /* the loading bar moves */
    if (page_focus(t->page) || t->editing || (t->find_on && t->find_focus)) {
        uint32_t ph = uihost_ms() / 500;
        if (ph != t->blink) { t->blink = ph; redraw = 1; }
    }
    return redraw;
}

/* the pointer over the window: hover styles and mouse events for the page, and the link it is on */
static void tf_hover(uiapp_t *a, int px, int py)
{
    tf_t *t = a->st;
    tf_page *pg = shown(t);
    if (!pg) return;
    py -= tabs_h(t);
    int in = px >= 0 && py >= TB_H && py < TB_H + t->view_h && !(t->dl_text[0] && py < TB_H + DL_H);
    if (page_hover(pg, in ? px : -1, in ? py - TB_H + t->scroll : -1)) a->dirty = 1;
    const char *href = in ? page_link_at(pg, px, py - TB_H + t->scroll) : 0;
    char want[URL_MAX] = "";
    if (href && strncmp(href, "javascript:", 11) && href[0] != '#') page_resolve(page_url(pg), href, want, sizeof(want));
    if (strcmp(want, t->status)) { strcpy(t->status, want); a->dirty = 1; }
}

static void tfire_session_restore(uiapp_t *a);
static void tfire_session_save(uiapp_t *a);

static void tf_init(uiapp_t *a, const char *arg)
{
    tf_t *t = a->st;
    t->app = a;
    t->hpos = -1;
    t->ntabs = 1;
    t->view_w = 900;
    t->view_h = 560;
    t->sug_sel = -1;
    store_init();
    /* A local HTML/SVG document dropped on tFire is parsed as a page too. */
    if (arg && arg[0] == '/' && !strcmp(ui_file_kind(ui_basename(arg), 0), "web")) {
        uint8_t *html = 0;
        uint32_t len = 0;
        if (uihost_read(arg, &html, &len) == 0 && len <= 0x7FFFFFFFu) {
            t->page = page_open_html(arg, (const char *)html, (int)len, &host);
            free(html);
            if (t->page) {
                strncpy(t->url, arg, sizeof(t->url) - 1);
                t->url[sizeof(t->url) - 1] = 0;
                field_reset(t);
                strcpy(t->hist[0], t->url);
                t->nhist = 1;
                t->hpos = 0;
                t->load_start = uihost_ms();
                set_title(a, t);
                return;
            }
        } else if (html) free(html);
    }
    go(a, t, arg && arg[0] ? arg : HOME, 1);
    if (!arg || !arg[0]) tfire_session_restore(a);                 /* the tabs that were open when it was closed */
}

static void tf_close(uiapp_t *a)
{
    tf_t *t = a->st;
    tfire_session_save(a);
    tab_free(&t->tab);
    for (int i = 0; i < t->ntabs; i++)
        if (t->saved[i]) { tab_free(t->saved[i]); free(t->saved[i]); t->saved[i] = 0; }
}

/* the open tabs, kept when the window closes and opened again by tfire_session_restore (the desktop version does both) */
static void tfire_session_save(uiapp_t *a)
{
    tf_t *t = a->st;
    sb_t b = { 0 };
    int cur = 0, n = 0;
    for (int i = 0; i < t->ntabs; i++) {
        const char *u = i == t->cur ? t->url : t->saved[i]->url;
        if (!u[0]) continue;
        if (i == t->cur) cur = n;
        sb_add(&b, u);
        sb_add(&b, "\n");
        n++;
    }
    if (!b.bad && n) {
        sb_t h = { 0 };
        sb_add(&h, "cur ");
        sb_int(&h, cur);
        sb_add(&h, "\n");
        sb_add(&h, b.p);
        if (!h.bad) {
            char dir[520], path[520];
            store_dir(dir, sizeof(dir));
            uihost_mkdir(dir);
            store_path("session.txt", path, sizeof(path));
            uihost_write(path, h.p, (uint32_t)h.len);
        }
        free(h.p);
    }
    free(b.p);
}

static void tfire_session_restore(uiapp_t *a)
{
    tf_t *t = a->st;
    char path[520];
    store_path("session.txt", path, sizeof(path));
    uint8_t *d = 0;
    uint32_t len = 0;
    if (uihost_read(path, &d, &len) != 0 || !d) return;
    static char urls[MAX_TABS][URL_MAX];
    int n = 0, cur = 0, cur_raw = 0, raw = 0;
    uint32_t i = 0;
    while (i < len) {
        uint32_t e = i;
        while (e < len && d[e] != '\n') e++;
        char line[URL_MAX];
        uint32_t l = e - i < sizeof(line) - 1 ? e - i : sizeof(line) - 1;
        memcpy(line, d + i, l);
        line[l] = 0;
        if (starts(line, "cur ")) cur_raw = parse_int(line + 4);
        else {
            if (n < MAX_TABS && (recordable(line) || !strcmp(line, HOME) || !strcmp(line, "tfire:bookmarks") || !strcmp(line, "tfire:history") || !strcmp(line, "tfire:licenses"))) {
                if (raw == cur_raw) cur = n;                 /* (a line that is not kept moves the ones after it) */
                strcpy(urls[n++], line);
            }
            raw++;
        }
        i = e + 1;
    }
    free(d);
    if (!n) return;
    tab_free(&t->tab);
    memset(&t->tab, 0, sizeof(tab_t));
    t->hpos = -1;
    go(a, t, urls[0], 1);
    for (int k = 1; k < n; k++) tab_new(a, t, urls[k], 0);
    tab_switch(a, t, cur < t->ntabs ? cur : 0);
}

const uiapp_class_t app_tfire = {
    "tfire", "tFire", "tfire", 960, 680, 1, sizeof(tf_t),
    tf_init, tf_draw, tf_press, 0, 0, tf_key, tf_wheel, tf_tick, tf_close,
    0, 0, 0,
    tf_hover,
    0,
};
