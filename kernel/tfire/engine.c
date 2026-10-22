/*
 * tFire's engine: a page's life, from its address to what is drawn (see engine.h).
 *
 * Everything here runs on a stack of its own (2 MB): the HTML parser, the style engine and QuickJS all
 * recurse further than a desktop task's stack allows.
 */
#include "engine.h"
#include "anim.h"
#include "tfire_libc.h"
#include "webfetch.h"
#include "uiapp.h"
#include "uihost.h"
#include "images.h"

extern void serial_write(const char *data);
extern void memory_get_usage(uint32_t *total_kb, uint32_t *used_kb);

#define URL_CAP 1024
#define MAX_IMAGES 600
#define IMG_BYTES_MAX (96u << 20)        /* decoded pictures of one page, at most */
#define IMG_SIDE_MAX 1600
#define RESTYLE_MS 200                   /* while scripts keep changing the page: style and layout this often */
#define CONSOLE_LINES 200

enum { R_CSS, R_SCRIPT, R_MODULE, R_XHR, R_IMG };
enum { S_WAIT, S_FETCHING, S_READY, S_FAILED, S_DONE };

/* a style sheet of the page: a <style>, or a <link rel=stylesheet> and the text fetched for it */
typedef struct {
    char *url;
    char *css;
    int len;
    int state;
} css_t;

/* a script waiting its turn: in document order, or (inserted later by a script) whenever it is ready */
typedef struct {
    tf_node *node;
    char *url;                           /* 0: inline */
    char *src;
    int len;
    int state;
    int module;
    int ordered;                         /* one of the page's own: run in document order */
} script_t;

typedef struct {
    char *url;
    int state;
    tf_picture pic;
    int want_w, want_h;                  /* the size the page gives it, if it does */
    tf_node *notify;                     /* the first <img> that asked (its load event) */
} image_t;

/* a fetch() or XMLHttpRequest */
typedef struct {
    int id;
    char *method, *url, *headers, *body;
    int body_len;
    int state;
} xhr_t;

struct tf_page {
    tf_host host;
    tf_request req;                      /* what was asked for */
    char url[URL_CAP];                   /* where the page is (after redirects, pushState) */
    char title[128];
    void *docfetch;
    int phase;                           /* 0 fetching the page, 1 it is here, 2 failed */
    int loading;
    tf_doc *doc;
    tf_styler *styler;
    tf_layout L;
    tf_js *js;
    int vw, vh, laid_w;
    int need_style, need_layout, need_sheets;
    uint32_t last_style, last_layout;
    int styling, style_rate;               /* styling a slice at a time (style_compute_step); elements a millisecond */
    int styled_once;
    int layout_soon;                       /* styles changed: lay out on the next tick (not after pictures' half second) */
    int style_soon;                        /* the pointer moved onto something :hover cares about: style now */
    /* what moves (anim.c) */
    tf_anims *anims;
    int anim_moving;
    uint32_t last_frame, layout_ms;
    /* what it needs */
    css_t *css; int ncss;
    script_t *scripts; int nscripts;
    image_t imgs[MAX_IMAGES]; int nimgs;
    uint32_t img_bytes;
    xhr_t *xhrs; int nxhrs;
    struct { char *url; int state; } mods[32]; int nmods;
    /* the one fetch running */
    void *fetch;
    int fetch_kind, fetch_idx;
    /* events */
    int dcl_fired, load_fired;
    uint32_t next_timer;                 /* uihost_ms() when js_tick is due; 0 = none */
    tf_node *focus;
    tf_node *hover;                    /* the element under the pointer */
    uint32_t caret_t0;
    tf_node *writing_after;              /* document.write: where the script that is running is */
    /* what the window is asked */
    int has_nav;
    tf_request nav;
    int has_url_change;
    /* a download (page_take_download) */
    int has_download;
    char dl_name[128];
    void *dl_raw;
    const uint8_t *dl_data;
    uint32_t dl_len;
    int scroll_req;
    int scroll_y;
    char *console[CONSOLE_LINES];
    int ncons;
};

static tf_page *current;                 /* the page whose engine is running (for printing from QuickJS) */

/* ------------------------------------------------------------------ a stack of its own */

static uint8_t *big_stack;
static int on_big_stack;
#define BIG_STACK (2u << 20)

static void (*big_fn)(void *);
static void *big_arg;
static void big_entry(void) __attribute__((used));
static void big_entry(void) { big_fn(big_arg); }

static void run_big(void (*fn)(void *), void *arg)
{
    if (on_big_stack) { fn(arg); return; }
    if (!big_stack) big_stack = malloc(BIG_STACK);
    if (!big_stack) { fn(arg); return; }
    uint32_t top = ((uint32_t)(uintptr_t)big_stack + BIG_STACK - 16) & ~15u;
    on_big_stack = 1;
    big_fn = fn;
    big_arg = arg;
    /* the old stack pointer is kept at the top of the new stack, and taken back from there */
    __asm__ volatile(
        "movl %%esp, %%esi\n\t"
        "movl %0, %%esp\n\t"
        "subl $12, %%esp\n\t"
        "pushl %%esi\n\t"
        "call big_entry\n\t"
        "movl (%%esp), %%esp\n\t"
        : : "r"(top) : "eax", "ecx", "edx", "esi", "memory", "cc");
    on_big_stack = 0;
    big_fn = 0;
    big_arg = 0;                        /* (it pointed at the caller's locals) */
}

/* ------------------------------------------------------------------ small things */

static char *dupn(const char *s, int n)
{
    char *d = malloc((size_t)n + 1);
    if (d) { memcpy(d, s, (size_t)n); d[n] = 0; }
    return d;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static int istarts(const char *s, const char *p)
{
    for (; *p; s++, p++) if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    return 1;
}

/* a word in a space-separated attribute value (rel="stylesheet preload") */
static int has_word(const char *v, const char *w)
{
    size_t wl = strlen(w);
    while (v && *v) {
        while (*v == ' ' || *v == '\t' || *v == '\n') v++;
        const char *s = v;
        while (*v && *v != ' ' && *v != '\t' && *v != '\n') v++;
        if ((size_t)(v - s) == wl && !strncasecmp(s, w, wl)) return 1;
    }
    return 0;
}

uint64_t tfire_now_ms(void)
{
    int y, mo, d, h, mi, s;
    uihost_time(&y, &mo, &d, &h, &mi, &s);
    /* days since 1970 (civil from days, Howard Hinnant's) */
    int yy = mo <= 2 ? y - 1 : y;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint64_t)(((days * 24 + h) * 60 + mi) * 60 + s) * 1000 + uihost_ms() % 1000;
}

void tfire_console_write(const char *s, int len)
{
    char b[300];
    if (len > 299) len = 299;
    memcpy(b, s, (size_t)len);
    b[len] = 0;
    if (current) page_console(current, 0, b);
}

/* ------------------------------------------------------------------ addresses */

int page_resolve(const char *base, const char *href, char *out, int cap)
{
    while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r') href++;
    char h[2048];
    int hl = 0;
    for (const char *c = href; *c && hl < (int)sizeof(h) - 4; c++) {
        if (*c == '\n' || *c == '\r' || *c == '\t') continue;
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7F) {     /* spaces and non-ASCII: percent-encoded */
            h[hl++] = '%'; h[hl++] = "0123456789ABCDEF"[(unsigned char)*c >> 4]; h[hl++] = "0123456789ABCDEF"[*c & 15];
        } else h[hl++] = *c;
    }
    while (hl && h[hl - 1] == '%' ) hl--;
    h[hl] = 0;
    href = h;
    if (istarts(href, "http://") || istarts(href, "https://")) { strncpy(out, href, (size_t)cap - 1); out[cap - 1] = 0; return 1; }
    if (istarts(href, "javascript:") || istarts(href, "mailto:") || istarts(href, "tel:") || istarts(href, "data:") ||
        istarts(href, "blob:") || istarts(href, "about:") || istarts(href, "tfire:")) return 0;
    const char *hs = strstr(base, "://");
    if (!hs) return 0;
    hs += 3;
    const char *pe = hs;
    while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    int origin = (int)(pe - base);
    char tmp[4096];
    if (href[0] == '/' && href[1] == '/') {
        int sl = (int)(hs - 2 - base);
        snprintf(tmp, sizeof(tmp), "%.*s%s", sl, base, href);
    } else if (href[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%.*s%s", origin, base, href);
    } else if (href[0] == '?' || href[0] == '#' || !href[0]) {
        const char *q = pe;
        while (*q && *q != (href[0] == '#' ? '#' : '?') && (href[0] == '#' || *q != '#')) q++;
        snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(q - base), base, href);
    } else {
        const char *q = pe;
        while (*q && *q != '?' && *q != '#') q++;
        const char *slash = 0;
        for (const char *c = pe; c < q; c++) if (*c == '/') slash = c;
        if (slash) snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(slash + 1 - base), base, href);
        else snprintf(tmp, sizeof(tmp), "%.*s/%s", origin, base, href);
    }
    /* "./" and "../" folded; scheme and host lower case */
    char *path = strstr(tmp, "://");
    path = path ? path + 3 : tmp;
    for (char *c = tmp; c < path; c++) *c = (char)tolower((unsigned char)*c);
    while (*path && *path != '/' && *path != '?' && *path != '#') { *path = (char)tolower((unsigned char)*path); path++; }
    char o[4096];
    int pl = (int)(path - tmp), n = pl;
    memcpy(o, tmp, (size_t)pl);
    const char *c = path;
    while (*c && *c != '?' && *c != '#' && n < (int)sizeof(o) - 2) {
        if (starts(c, "/./")) { c += 2; continue; }
        if (!strcmp(c, "/.") || starts(c, "/.?") || starts(c, "/.#")) { o[n++] = '/'; c += 2; continue; }
        if (starts(c, "/../") || !strcmp(c, "/..") || starts(c, "/..?") || starts(c, "/..#")) {
            while (n > pl && o[n - 1] != '/') n--;
            if (n > pl) n--;
            c += 3;
            if (!*c || *c == '?' || *c == '#') o[n++] = '/';
            continue;
        }
        o[n++] = *c++;
    }
    if (n == pl) o[n++] = '/';
    while (*c && n < (int)sizeof(o) - 1) o[n++] = *c++;
    o[n] = 0;
    if (n >= cap) return 0;
    strcpy(out, o);
    return 1;
}

/* ------------------------------------------------------------------ the console */

void page_console(tf_page *p, int level, const char *msg)
{
    char line[512];
    snprintf(line, sizeof(line), "[tfire%s] %s\n", level >= 2 ? " error" : level == 1 ? " warn" : "", msg);
    serial_write(line);
    if (p->ncons == CONSOLE_LINES) { free(p->console[0]); memmove(p->console, p->console + 1, sizeof(char *) * (CONSOLE_LINES - 1)); p->ncons--; }
    p->console[p->ncons++] = strdup(line);
}

/* ------------------------------------------------------------------ storage (per origin, while tOS runs) */

typedef struct store { struct store *next; char *origin; void *session; char *key, *value; } store_t;
static store_t *stores;

static void origin_of(const char *url, char *out, int cap)
{
    const char *h = strstr(url, "://");
    int n = 0;
    if (h) {
        h += 3;
        while (*h && *h != '/' && *h != '?' && *h != '#') h++;
        n = (int)(h - url);
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, url, (size_t)n);
    out[n] = 0;
}

static store_t **store_find(tf_page *p, int session, const char *key)
{
    char org[256];
    origin_of(p->url, org, sizeof(org));
    void *sid = session ? p->host.ctx : 0;
    for (store_t **s = &stores; *s; s = &(*s)->next)
        if ((*s)->session == sid && !strcmp((*s)->origin, org) && (!key || !strcmp((*s)->key, key))) return s;
    return 0;
}

char *page_storage_get(tf_page *p, int session, const char *key)
{
    store_t **s = store_find(p, session, key);
    return s ? strdup((*s)->value) : 0;
}

void page_storage_set(tf_page *p, int session, const char *key, const char *value)
{
    store_t **s = store_find(p, session, key);
    if (!value) {
        if (s) { store_t *g = *s; *s = g->next; free(g->origin); free(g->key); free(g->value); free(g); }
        return;
    }
    if (s) { free((*s)->value); (*s)->value = strdup(value); return; }
    store_t *n = calloc(1, sizeof(*n));
    if (!n) return;
    char org[256];
    origin_of(p->url, org, sizeof(org));
    n->origin = strdup(org);
    n->session = session ? p->host.ctx : 0;
    n->key = strdup(key);
    n->value = strdup(value);
    n->next = stores;
    stores = n;
}

int page_storage_keys(tf_page *p, int session, char ***keys)
{
    char org[256];
    origin_of(p->url, org, sizeof(org));
    void *sid = session ? p->host.ctx : 0;
    int n = 0, cap = 0;
    *keys = 0;
    for (store_t *s = stores; s; s = s->next) {
        if (s->session != sid || strcmp(s->origin, org)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; char **nk = realloc(*keys, (size_t)cap * sizeof(char *)); if (!nk) break; *keys = nk; }
        (*keys)[n++] = strdup(s->key);
    }
    return n;
}

void page_storage_clear(tf_page *p, int session)
{
    for (store_t **s = store_find(p, session, 0); s; s = store_find(p, session, 0)) {
        store_t *g = *s;
        *s = g->next;
        free(g->origin); free(g->key); free(g->value); free(g);
    }
}

/* ------------------------------------------------------------------ what the document needs */

static int css_index(tf_page *p, const char *url)
{
    for (int i = 0; i < p->ncss; i++) if (p->css[i].url && !strcmp(p->css[i].url, url)) return i;
    return -1;
}

static void css_want(tf_page *p, const char *url)
{
    if (css_index(p, url) >= 0) return;
    css_t *n = realloc(p->css, (size_t)(p->ncss + 1) * sizeof(css_t));
    if (!n) return;
    p->css = n;
    memset(&n[p->ncss], 0, sizeof(css_t));
    n[p->ncss].url = strdup(url);
    n[p->ncss].state = S_WAIT;
    p->ncss++;
    p->loading = 1;
}

static int is_js_type(const char *type, int *module)
{
    *module = 0;
    if (!type || !*type) return 1;
    if (!strcasecmp(type, "module")) { *module = 1; return 1; }
    return istarts(type, "text/javascript") || istarts(type, "application/javascript") || istarts(type, "text/ecmascript") ||
           istarts(type, "application/ecmascript") || istarts(type, "application/x-javascript") || istarts(type, "text/jscript");
}

static void script_add(tf_page *p, tf_node *n, int ordered)
{
    int module;
    if (n->started || !is_js_type(dom_attr(n, "type"), &module)) return;
    if (dom_attr(n, "nomodule")) return;                 /* tFire runs modules, so their stand-ins are left out */
    const char *src = dom_attr(n, "src");
    char url[URL_CAP];
    int ext = src && *src && page_resolve(p->url, src, url, sizeof(url));
    if (src && *src && !ext) return;
    if (!src || !*src) {
        char *t = dom_text_content(n);
        if (!t) return;
        int blank = 1;
        for (char *c = t; *c && blank; c++) if (*c != ' ' && *c != '\n' && *c != '\r' && *c != '\t') blank = 0;
        if (blank) { free(t); return; }                /* an empty <script> a script fills in later */
        free(t);
    }
    script_t *s = realloc(p->scripts, (size_t)(p->nscripts + 1) * sizeof(script_t));
    if (!s) return;
    p->scripts = s;
    s = &s[p->nscripts++];
    memset(s, 0, sizeof(*s));
    n->started = 1;
    s->node = n;
    s->module = module;
    s->ordered = ordered;
    if (ext) { s->url = strdup(url); s->state = S_WAIT; p->loading = 1; }
    else {
        s->src = dom_text_content(n);
        s->len = s->src ? (int)strlen(s->src) : 0;
        s->state = S_READY;
    }
}

/* a media="" list: does one of its queries take a screen? (the sizes in it are left to an @media around the sheet) */
static int media_ok(const char *media)
{
    if (!media || !*media) return 1;
    const char *q = media;
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        const char *e = q;
        int depth = 0;
        while (*e && (depth || *e != ',')) { if (*e == '(') depth++; else if (*e == ')') depth--; e++; }
        const char *w = q;
        int neg = 0;
        if (!strncasecmp(w, "not ", 4)) { neg = 1; w += 4; } else if (!strncasecmp(w, "only ", 5)) w += 5;
        while (*w == ' ') w++;
        int screen = *w == '(' || !strncasecmp(w, "screen", 6) || !strncasecmp(w, "all", 3);
        if (screen != neg) return 1;
        q = e;
    }
    return 0;
}

static int link_is_sheet(tf_node *n)
{
    const char *rel = dom_attr(n, "rel"), *media = dom_attr(n, "media");
    if (!rel || !has_word(rel, "stylesheet") || has_word(rel, "alternate") || dom_attr(n, "disabled")) return 0;
    return media_ok(media);
}

/* a sheet with media="(...)" sizes in it: inside an @media of them */
static void add_sheet_media(tf_styler *s, const char *css, size_t l, const char *media, const char *base)
{
    if (media && strchr(media, '(') && !strchr(media, ',') && strncasecmp(media, "not ", 4)) {
        size_t ml = strlen(media);
        char *w = malloc(l + ml + 16);
        if (w) {
            int n = sprintf(w, "@media %s{", media);
            memcpy(w + n, css, l);
            w[n + l] = '}';
            w[n + l + 1] = 0;
            style_add_sheet(s, w, (uint32_t)(n + l + 1), base);
            free(w);
            return;
        }
    }
    style_add_sheet(s, css, (uint32_t)l, base);
}

/* a node is now in the document: its scripts, sheets and pictures are wanted */
static void scan(tf_page *p, tf_node *top, int ordered)
{
    for (tf_node *n = top; n; n = dom_next(n, top)) {
        if (n->type != TF_ELEM) continue;
        if (n->ns != 1 && n->ns != 0 && !dom_is(n, "style")) continue;
        if (dom_is(n, "script")) script_add(p, n, ordered);
        else if (dom_is(n, "link") && link_is_sheet(n)) {
            char url[URL_CAP];
            if (page_resolve(p->url, dom_attr(n, "href") ? dom_attr(n, "href") : "", url, sizeof(url))) css_want(p, url);
            p->need_sheets = 1;
        } else if (dom_is(n, "style")) p->need_sheets = 1;
        else if (dom_is(n, "img")) n->img = 0;                 /* (looked at again: its address may be new) */
        else if (dom_is(n, "base") && dom_attr(n, "href")) {
            char url[URL_CAP];
            if (page_resolve(p->url, dom_attr(n, "href"), url, sizeof(url))) { /* links resolve against it from now on */ }
        }
    }
}

void page_node_inserted(tf_page *p, tf_node *n)
{
    if (!p->doc || !dom_connected(p->doc, n)) return;
    scan(p, n, 0);
    p->need_style = 1;
}

/* ------------------------------------------------------------------ style sheets in, styles out */

static void rebuild_sheets(tf_page *p)
{
    tf_styler *s = style_new();
    if (!s) return;
    {
        char cls[1024] = "";
        tf_node *html = dom_find(p->doc->root, "html"), *body = html ? dom_find(html, "body") : 0;
        snprintf(cls, sizeof(cls), "%s %s", html && dom_attr(html, "class") ? dom_attr(html, "class") : "",
                 body && dom_attr(body, "class") ? dom_attr(body, "class") : "");
        style_set_root_classes(s, cls);
        char at[2048];
        int o = 0;
        at[0] = 0;
        tf_node *two[2] = { html, body };
        for (int k = 0; k < 2; k++)
            for (int i = 0; two[k] && i < two[k]->nattrs && o < (int)sizeof(at) - 200; i++) {
                const tf_attr *a = &two[k]->attrs[i];
                o += snprintf(at + o, sizeof(at) - (size_t)o, "%s=%.150s\n", lwc_string_data(a->name), a->value ? a->value : "");
            }
        style_set_root_attrs(s, at);
    }
    for (tf_node *n = p->doc->root; n; n = dom_next(n, p->doc->root)) {
        if (n->type != TF_ELEM) continue;
        if (dom_is(n, "style")) {
            const char *media = dom_attr(n, "media");
            if (!media_ok(media)) continue;
            char *css = dom_text_content(n);
            if (css) { add_sheet_media(s, css, strlen(css), media, p->url); free(css); }
        } else if (dom_is(n, "link") && link_is_sheet(n)) {
            char url[URL_CAP];
            if (!page_resolve(p->url, dom_attr(n, "href") ? dom_attr(n, "href") : "", url, sizeof(url))) continue;
            int i = css_index(p, url);
            if (i >= 0 && p->css[i].state == S_READY) add_sheet_media(s, p->css[i].css, (size_t)p->css[i].len, dom_attr(n, "media"), url);
        }
    }
    style_free(p->styler);
    p->styler = s;
    p->need_sheets = 0;
    p->need_style = 1;
}

static int css_pending(tf_page *p)
{
    for (int i = 0; i < p->ncss; i++) if (p->css[i].state == S_WAIT || p->css[i].state == S_FETCHING) return 1;
    return 0;
}

/* ------------------------------------------------------------------ pictures */

static int image_size_cb(tf_node *img, int *w, int *h);
static tf_page *layout_page;               /* the page being laid out (for image_size_cb) */

/* the address a picture should come from: src, or what a lazy loader or srcset would give */
static int img_url(tf_page *p, tf_node *n, char *out, int cap)
{
    const char *src = dom_attr(n, "src");
    int placeholder = !src || !*src || (istarts(src, "data:") && strlen(src) < 200);
    const char *alt[] = { "data-src", "data-lazy-src", "data-original", "data-lazy", "data-url", 0 };
    for (int i = 0; placeholder && alt[i]; i++) { const char *a = dom_attr(n, alt[i]); if (a && *a) { src = a; placeholder = 0; } }
    const char *ss = 0;
    if (placeholder) ss = dom_attr(n, "srcset");
    if (placeholder && (!ss || !*ss)) ss = dom_attr(n, "data-srcset");
    if (placeholder && (!ss || !*ss) && n->parent && dom_is(n->parent, "picture"))
        for (tf_node *s = n->parent->first; s && (!ss || !*ss); s = s->next) if (dom_is(s, "source")) ss = dom_attr(s, "srcset");
    char first[URL_CAP];
    if (placeholder && ss && *ss) {
        while (*ss == ' ') ss++;
        int k = 0;
        while (ss[k] && ss[k] != ' ' && ss[k] != ',' && k < URL_CAP - 1) { first[k] = ss[k]; k++; }
        first[k] = 0;
        if (k) { src = first; placeholder = 0; }
    }
    if (!src || !*src) return 0;
    if (istarts(src, "data:")) { strncpy(out, src, (size_t)cap - 1); out[cap - 1] = 0; return 1; }
    return page_resolve(p->url, src, out, cap);
}

/* a picture of url in the page's list (added if new); its index plus one, or -1 */
static int image_slot(tf_page *p, const char *url, tf_node *notify, int want_w, int want_h)
{
    int i;
    for (i = 0; i < p->nimgs; i++) if (!strcmp(p->imgs[i].url, url)) return i + 1;
    if (p->nimgs == MAX_IMAGES) return -1;
    image_t *im = &p->imgs[p->nimgs++];
    memset(im, 0, sizeof(*im));
    im->url = strdup(url);
    im->state = S_WAIT;
    im->notify = notify;
    im->want_w = want_w;
    im->want_h = want_h;
    return p->nimgs;
}

/* mask-image: url(...) -> the address it names */
static int mask_url(tf_page *p, const char *v, char *out, int cap)
{
    const char *u = strstr(v, "url(");
    if (!u) return 0;
    u += 4;
    while (*u == ' ') u++;
    char q = (*u == '"' || *u == '\'') ? *u++ : 0;
    char tmp[URL_CAP];
    int n = 0;
    while (*u && n < URL_CAP - 1 && (q ? *u != q : (*u != ')' && *u != ' '))) tmp[n++] = *u++;
    tmp[n] = 0;
    if (!n) return 0;
    if (istarts(tmp, "data:")) { strncpy(out, tmp, (size_t)cap - 1); out[cap - 1] = 0; return 1; }
    return page_resolve(p->url, tmp, out, cap);
}

/* an <svg> in the page drawn as a picture of its own, in the colour its text has (currentColor) */
static void inline_svg(tf_page *p, tf_node *n)
{
    n->img = -1;
    if (p->nimgs == MAX_IMAGES || p->img_bytes > IMG_BYTES_MAX) return;
    char *src = dom_outer_html(n);
    if (!src) return;
    char col[8];
    uint32_t c = n->style->color;
    snprintf(col, sizeof(col), "#%06x", (unsigned)(c & 0xFFFFFF));
    /* currentColor -> the colour; and a fill for shapes that have none (icons are drawn in it) */
    size_t l = strlen(src);
    char *o = malloc(l * 2 + 64);
    if (!o) { free(src); return; }
    size_t k = 0;
    int root_done = 0;
    for (size_t i = 0; i < l;) {
        if (!strncasecmp(src + i, "currentcolor", 12)) { memcpy(o + k, col, 7); k += 7; i += 12; continue; }
        if (!root_done && !strncmp(src + i, "<svg", 4)) {
            memcpy(o + k, "<svg", 4); k += 4; i += 4;
            if (!strstr(src, " fill=")) { k += (size_t)sprintf(o + k, " fill=\"%s\"", col); }
            if (!strstr(src, "xmlns")) { memcpy(o + k, " xmlns=\"http://www.w3.org/2000/svg\"", 35); k += 35; }
            root_done = 1;
            continue;
        }
        o[k++] = src[i++];
    }
    o[k] = 0;
    free(src);
    const char *aw = dom_attr(n, "width"), *ah = dom_attr(n, "height");
    int ww = aw ? atoi(aw) : 0, wh = ah ? atoi(ah) : 0;
    if (!n->style->width.autov && !n->style->width.pct) ww = n->style->width.v;
    if (!n->style->height.autov && !n->style->height.pct) wh = n->style->height.v;
    uint8_t *px;
    int w, h;
    if (tf_svg_decode((const uint8_t *)o, (uint32_t)k, ww, wh, IMG_SIDE_MAX, IMG_SIDE_MAX, &px, &w, &h) == 0) {
        image_t *im = &p->imgs[p->nimgs++];
        memset(im, 0, sizeof(*im));
        im->url = strdup("svg:inline");
        im->state = S_READY;
        im->pic.rgba = px; im->pic.w = w; im->pic.h = h; im->pic.dw = (w + 1) / 2; im->pic.dh = (h + 1) / 2;
        p->img_bytes += (uint32_t)(w * h * 4);
        n->img = p->nimgs;
    }
    free(o);
}

static void images_want(tf_page *p)
{
    for (tf_node *n = p->doc->root; n; n = dom_next(n, p->doc->root)) {
        if (n->type == TF_ELEM && !n->img && n->style && n->style->display != TD_NONE && dom_is(n, "svg")) { inline_svg(p, n); continue; }
        if (n->type == TF_ELEM && n->style && n->style->mask && !n->mimg && n->style->display != TD_NONE) {
            char url[URL_CAP];
            int ww = !n->style->width.autov && !n->style->width.pct ? n->style->width.v : 20;
            int wh = !n->style->height.autov && !n->style->height.pct ? n->style->height.v : 20;
            n->mimg = mask_url(p, n->style->mask, url, sizeof(url)) ? image_slot(p, url, 0, ww, wh) : -1;
        }
        if (!dom_is(n, "img") || n->img || !n->style || n->style->display == TD_NONE) continue;
        char url[URL_CAP];
        if (!img_url(p, n, url, sizeof(url))) { n->img = -1; continue; }
        int i;
        for (i = 0; i < p->nimgs; i++) if (!strcmp(p->imgs[i].url, url)) break;
        if (i == p->nimgs) {
            if (p->nimgs == MAX_IMAGES) { n->img = -1; continue; }
            image_t *im = &p->imgs[p->nimgs++];
            memset(im, 0, sizeof(*im));
            im->url = strdup(url);
            im->state = S_WAIT;
            im->notify = n;
            const char *w = dom_attr(n, "width"), *h = dom_attr(n, "height");
            im->want_w = w ? atoi(w) : 0;
            im->want_h = h ? atoi(h) : 0;
        }
        n->img = i + 1;
    }
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void image_decode(tf_page *p, image_t *im, const uint8_t *d, uint32_t len)
{
    im->state = S_FAILED;
    if (p->img_bytes > IMG_BYTES_MAX) return;
    int mw = IMG_SIDE_MAX, mh = IMG_SIDE_MAX;
    if (im->want_w > 0 && im->want_w < mw) { mw = im->want_w * 2 < IMG_SIDE_MAX ? im->want_w * 2 : IMG_SIDE_MAX; }
    uint8_t *px = 0;
    int w = 0, h = 0, dw = 0, dh = 0, ok;
    switch (tf_image_kind(d, len)) {
    case TF_IMG_SVG:
        ok = tf_svg_decode(d, len, im->want_w, im->want_h, IMG_SIDE_MAX, IMG_SIDE_MAX, &px, &w, &h) == 0;
        dw = (w + 1) / 2;                       /* (drawn at twice the size it is shown at) */
        dh = (h + 1) / 2;
        break;
    case TF_IMG_WEBP:
        ok = tf_webp_decode(d, len, mw, mh, &px, &w, &h) == 0;
        dw = w; dh = h;
        break;
    default:
        ok = uihost_image_decode(d, len, mw, mh, &px, &w, &h) == 0;
        dw = w; dh = h;
        break;
    }
    if (!ok) return;
    im->pic.rgba = px; im->pic.w = w; im->pic.h = h; im->pic.dw = dw; im->pic.dh = dh;
    im->state = S_READY;
    p->img_bytes += (uint32_t)(w * h * 4);
}

static void image_data_url(tf_page *p, image_t *im)
{
    const char *c = strchr(im->url, ',');
    im->state = S_FAILED;
    if (!c) return;
    int b64 = 0;
    for (const char *q = im->url; q < c; q++) if (!strncmp(q, ";base64", 7)) b64 = 1;
    c++;
    size_t n = strlen(c);
    uint8_t *b = malloc(n + 4);
    if (!b) return;
    uint32_t o = 0;
    if (b64) {
        uint32_t acc = 0;
        int bits = 0;
        for (; *c; c++) {
            int v = b64v((unsigned char)*c);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; b[o++] = (uint8_t)(acc >> bits); }
        }
    } else {
        for (; *c; c++) {                       /* data:image/svg+xml,%3Csvg ... */
            if (*c == '%' && hexv(c[1]) >= 0 && hexv(c[2]) >= 0) { b[o++] = (uint8_t)(hexv(c[1]) * 16 + hexv(c[2])); c += 2; }
            else b[o++] = (uint8_t)*c;
        }
    }
    image_decode(p, im, b, o);
    free(b);
}

tf_picture *page_picture(tf_page *p, tf_node *img)
{
    if (!img || img->img <= 0 || img->img > p->nimgs) return 0;
    image_t *im = &p->imgs[img->img - 1];
    return im->state == S_READY ? &im->pic : 0;
}

tf_picture *page_mask(tf_page *p, tf_node *n)
{
    if (!n || n->mimg <= 0 || n->mimg > p->nimgs) return 0;
    image_t *im = &p->imgs[n->mimg - 1];
    return im->state == S_READY ? &im->pic : 0;
}

static int image_size_cb(tf_node *img, int *w, int *h)
{
    tf_picture *pic = layout_page ? page_picture(layout_page, img) : 0;
    if (!pic) { *w = *h = 0; return -1; }
    *w = pic->dw;
    *h = pic->dh;
    return 0;
}

/* ------------------------------------------------------------------ fetching, one at a time */

static int fetch_tries;

/* 1 = started; 0 = tOS could not start it now (no free fetch task): it is tried again on a later tick */
static int fetch_start(tf_page *p, int kind, int idx, const char *url, const char *method, const char *headers, const char *body, int blen)
{
    p->fetch = webfetch_request(url, method, headers, body, blen);
    p->fetch_kind = kind;
    p->fetch_idx = idx;
    if (p->fetch) { fetch_tries = 0; return 1; }
    char m[URL_CAP + 40];
    snprintf(m, sizeof(m), "could not start fetching %s (try %d)", url, fetch_tries + 1);
    page_console(p, 1, m);
    return 0;
}

/* a fetch that could not start: back to waiting, or given up on after many tries */
static int fetch_retry(int *state)
{
    if (++fetch_tries > 50) { *state = S_FAILED; fetch_tries = 0; return 1; }
    *state = S_WAIT;
    return 0;
}

/* the next thing to fetch, by priority; 0 = nothing waits */
static int fetch_next(tf_page *p)
{
    for (int i = 0; i < p->ncss; i++)
        if (p->css[i].state == S_WAIT) {
            p->css[i].state = S_FETCHING;
            if (fetch_start(p, R_CSS, i, p->css[i].url, 0, 0, 0, 0)) return 1;
            if (fetch_retry(&p->css[i].state)) p->need_sheets = 1;
            return 0;
        }
    for (int i = 0; i < p->nscripts; i++)
        if (p->scripts[i].state == S_WAIT) {
            p->scripts[i].state = S_FETCHING;
            if (fetch_start(p, R_SCRIPT, i, p->scripts[i].url, 0, 0, 0, 0)) return 1;
            fetch_retry(&p->scripts[i].state);
            return 0;
        }
    for (int i = 0; i < p->nmods; i++)
        if (p->mods[i].state == S_WAIT) {
            p->mods[i].state = S_FETCHING;
            if (fetch_start(p, R_MODULE, i, p->mods[i].url, 0, 0, 0, 0)) return 1;
            if (fetch_retry(&p->mods[i].state) && p->js) tfjs_module_source(p->js, p->mods[i].url, 0, 0);
            return 0;
        }
    for (int i = 0; i < p->nxhrs; i++)
        if (p->xhrs[i].state == S_WAIT) {
            xhr_t *x = &p->xhrs[i];
            x->state = S_FETCHING;
            if (fetch_start(p, R_XHR, i, x->url, x->method, x->headers, x->body, x->body_len)) return 1;
            if (fetch_retry(&x->state)) { x->state = S_DONE; if (p->js) tfjs_fetch_done(p->js, x->id, 0, x->url, 0, 0, 0, 0, "could not start"); }
            return 0;
        }
    if (!p->styled_once) return 0;
    for (int i = 0; i < p->nimgs; i++) {
        image_t *im = &p->imgs[i];
        if (im->state != S_WAIT) continue;
        if (istarts(im->url, "data:")) { image_data_url(p, im); p->need_layout = 1; if (p->js && im->notify) tfjs_image_loaded(p->js, im->notify, im->state == S_READY); continue; }
        im->state = S_FETCHING;
        if (fetch_start(p, R_IMG, i, im->url, 0, 0, 0, 0)) return 1;
        fetch_retry(&im->state);
        return 0;
    }
    return 0;
}

/* a fetch ended: its result goes where it belongs */
static void fetch_finish(tf_page *p)
{
    webfetch_t *f = p->fetch;
    int ok = f->state == WEBFETCH_DONE && f->status < 400;
    int i = p->fetch_idx;
    switch (p->fetch_kind) {
    case R_CSS:
        if (i < p->ncss) {
            css_t *c = &p->css[i];
            c->state = S_FAILED;
            if (ok) { c->css = dupn((const char *)f->body, (int)f->body_len); if (c->css) { c->len = (int)f->body_len; c->state = S_READY; } }
            p->need_sheets = 1;
        }
        break;
    case R_SCRIPT:
        if (i < p->nscripts) {
            script_t *s = &p->scripts[i];
            s->state = S_FAILED;
            if (ok) { s->src = dupn((const char *)f->body, (int)f->body_len); if (s->src) { s->len = (int)f->body_len; s->state = S_READY; } }
            if (s->state == S_FAILED) {
                char m[URL_CAP + 40];
                snprintf(m, sizeof(m), "could not fetch the script %s", s->url);
                page_console(p, 1, m);
                if (p->js) tfjs_event(p->js, s->node, "error", 0, 0, 0, 0);
            }
        }
        break;
    case R_MODULE:
        if (i < p->nmods) {
            p->mods[i].state = ok ? S_READY : S_FAILED;
            if (p->js) tfjs_module_source(p->js, p->mods[i].url, ok ? (const char *)f->body : 0, ok ? (int)f->body_len : 0);
        }
        break;
    case R_XHR:
        if (i < p->nxhrs) {
            xhr_t *x = &p->xhrs[i];
            x->state = S_DONE;
            if (p->js) {
                if (f->state == WEBFETCH_DONE)
                    tfjs_fetch_done(p->js, x->id, f->status, f->final_url[0] ? f->final_url : f->url, (const char *)f->raw, (int)f->head_len,
                                  (const char *)f->body, (int)f->body_len, 0);
                else tfjs_fetch_done(p->js, x->id, 0, x->url, 0, 0, 0, 0, f->error[0] ? f->error : "failed");
            }
        }
        break;
    case R_IMG:
        if (i < p->nimgs) {
            image_t *im = &p->imgs[i];
            im->state = S_FAILED;
            if (ok) image_decode(p, im, f->body, f->body_len);
#ifdef TFIRE_DEBUG
            if (im->state != S_READY) {
                char m[URL_CAP + 80];
                snprintf(m, sizeof(m), "picture not shown (status %d, %u bytes, %s): %s", f->status, (unsigned)f->body_len, f->ctype, im->url);
                page_console(p, 1, m);
            }
#endif
            p->need_layout = 1;
            if (p->js && im->notify) tfjs_image_loaded(p->js, im->notify, im->state == S_READY);
        }
        break;
    }
    webfetch_free(f);
    p->fetch = 0;
}

/* ------------------------------------------------------------------ scripts */

static void run_script(tf_page *p, int idx)
{
    script_t *s = &p->scripts[idx];
    s->state = S_DONE;
    if (!p->js || !s->src) return;
    tf_node *node = s->node;
    char *src = s->src;
    int len = s->len, module = s->module;
    char *url = s->url ? strdup(s->url) : 0;
    p->writing_after = node;
    int again = tfjs_run(p->js, src, len, url ? url : p->url, node, module);
    p->writing_after = 0;
    s = &p->scripts[idx];                  /* (the script may have added scripts: the array may have moved) */
    if (again) {
        /* a module whose imports are not here yet: they are fetched, and it runs again after */
        for (int k = 0; tfjs_missing(p->js, k); k++) {
            const char *u = tfjs_missing(p->js, k);
            int have = 0;
            for (int m = 0; m < p->nmods; m++) if (!strcmp(p->mods[m].url, u)) have = 1;
            if (!have && p->nmods < 32) { p->mods[p->nmods].url = strdup(u); p->mods[p->nmods].state = S_WAIT; p->nmods++; }
        }
        s->state = S_READY;
        s->ordered = 0;
        s->module = 2;                                        /* (waits for its imports) */
        p->loading = 1;
    } else if (url) tfjs_event(p->js, node, "load", 0, 0, 0, 0);
    free(url);
}

static int mods_pending(tf_page *p)
{
    for (int i = 0; i < p->nmods; i++) if (p->mods[i].state == S_WAIT || p->mods[i].state == S_FETCHING) return 1;
    return 0;
}

/* runs what can run now: the page's own scripts in order, others as they come; 1 = something ran */
static int scripts_step(tf_page *p)
{
    int ran = 0;
    for (int i = 0; i < p->nscripts; i++) {
        script_t *s = &p->scripts[i];
        if (s->state == S_DONE || s->state == S_FAILED) continue;
        if (s->module == 2) {
            if (mods_pending(p)) continue;
            s->module = 1;
        }
        if (s->state != S_READY) { if (s->ordered) break; continue; }
        if (s->ordered && css_pending(p)) break;            /* scripts see the page styled */
        run_script(p, i);
        ran = 1;
        s = &p->scripts[i];                                  /* (a script may have added scripts: the array moved) */
        if (s->ordered && s->state != S_DONE) break;
    }
    return ran;
}

static int scripts_pending(tf_page *p)
{
    for (int i = 0; i < p->nscripts; i++) if (p->scripts[i].ordered && p->scripts[i].state != S_DONE && p->scripts[i].state != S_FAILED) return 1;
    return 0;
}

void page_write(tf_page *p, const char *html, int len)
{
    /* document.write while a script of the page runs: what it writes goes in after that script */
    tf_node *at = p->writing_after;
    if (!p->doc) return;
    tf_node *tmp = dom_new_element(p->doc, "div");
    if (!tmp) return;
    dom_set_inner_html(p->doc, tmp, html, len);
    tf_node *parent = at && at->parent ? at->parent : dom_find(p->doc->root, "body");
    if (!parent) return;
    tf_node *ref = at && at->parent ? at->next : 0;
    tf_node *first = tmp->first;
    while (tmp->first) {
        tf_node *k = tmp->first;
        dom_insert_before(parent, k, ref);
        if (at && at->parent) at = k;
    }
    if (at && at->parent) p->writing_after = at;
    for (tf_node *k = first; k && k->parent == parent; k = k->next) { scan(p, k, 0); if (k == p->writing_after) break; }
    p->need_style = 1;
}

/* ------------------------------------------------------------------ the page's own fetches */

int page_fetch(tf_page *p, int id, const char *method, const char *url, const char *headers, const char *body, int body_len)
{
    xhr_t *n = realloc(p->xhrs, (size_t)(p->nxhrs + 1) * sizeof(xhr_t));
    if (!n) return -1;
    p->xhrs = n;
    xhr_t *x = &n[p->nxhrs++];
    memset(x, 0, sizeof(*x));
    x->id = id;
    x->method = strdup(method ? method : "GET");
    x->url = strdup(url);
    x->headers = headers ? strdup(headers) : 0;
    if (body && body_len > 0) { x->body = dupn(body, body_len); x->body_len = body_len; }
    x->state = S_WAIT;
    return 0;
}

void page_fetch_abort(tf_page *p, int id)
{
    for (int i = 0; i < p->nxhrs; i++)
        if (p->xhrs[i].id == id && p->xhrs[i].state == S_WAIT) p->xhrs[i].state = S_DONE;
}

/* ------------------------------------------------------------------ style and layout */

/* Styling: started, then done a slice at a time on each tick (a big page takes seconds in an emulator), so the
 * desktop keeps running; the old layout stays on the screen until the new styles are all there. */
static void restyle_finish(tf_page *p)
{
    p->styling = 0;
    dom_find_title(p->doc);
    strncpy(p->title, p->doc->title, sizeof(p->title) - 1);
    p->need_layout = 1;
    p->layout_soon = 1;
    p->last_style = uihost_ms();
    p->styled_once = 1;
    images_want(p);
    /* transitions start, animations come and go, and the values of the moment go into the styles */
    if (!p->anims) p->anims = anim_new();
    anim_restyled(p->anims, p->doc, p->styler, p->last_style);
    p->anim_moving = (anim_apply(p->anims, p->last_style) & 2) != 0;
}

static void restyle_begin(tf_page *p)
{
    if (!p->doc) return;
    if (p->need_sheets || !p->styler) rebuild_sheets(p);
    if (!p->styler) return;
    if (p->hover && !dom_connected(p->doc, p->hover)) p->hover = 0;
    style_set_state(p->styler, p->hover, p->focus && dom_connected(p->doc, p->focus) ? p->focus : 0);
    style_compute_begin(p->styler, p->doc, p->vw, p->vh);
    p->styling = 1;
    p->need_style = 0;
    p->doc->changed = 0;
}

/* about budget ms of styling (0 = all of it); 1 when it is done */
static int restyle_step(tf_page *p, int budget)
{
    if (!p->styling) return 1;
    int n = budget ? p->style_rate * budget : 0;
    if (budget && n < 50) n = 50;
    uint32_t t0 = uihost_ms();
    int done = style_compute_step(p->styler, n);
    uint32_t dt = uihost_ms() - t0;
    if (budget && n) {
        /* how many elements fit in a millisecond, from this slice (smoothed) */
        int rate = (int)(n / (dt ? dt : 1));
        if (rate < 1) rate = 1;
        p->style_rate = (p->style_rate * 3 + rate) / 4;
        if (p->style_rate < 1) p->style_rate = 1;
    }
    if (done) restyle_finish(p);
    return done;
}

static void restyle(tf_page *p)
{
    restyle_begin(p);
    restyle_step(p, 0);
}

static void relayout(tf_page *p)
{
    if (!p->doc || !p->styled_once) return;
    uint32_t t0 = uihost_ms();
    tf_env env = p->host.env;
    env.image_size = image_size_cb;
    layout_page = p;
    layout_run(&p->L, p->doc, p->vw, p->vh, &env);
    layout_page = 0;
    p->laid_w = p->vw;
    p->need_layout = 0;
    p->layout_soon = 0;
    p->last_layout = uihost_ms();
    p->layout_ms = p->last_layout - t0;
#ifdef TFIRE_DEBUG
    uint32_t sm, bg;
    tf_alloc_usage(&sm, &bg);
    char m[200];
    uint32_t tk, uk;
    memory_get_usage(&tk, &uk);
    snprintf(m, sizeof(m), "layout: %d items, %dx%d, %u ms; memory %u KB small + %u KB big; kernel heap %u/%u KB", p->L.n, p->L.doc_w, p->L.doc_h,
             (unsigned)(uihost_ms() - t0), (unsigned)(sm >> 10), (unsigned)(bg >> 10), (unsigned)uk, (unsigned)tk);
    page_console(p, 0, m);
#else
    (void)t0;
#endif
}

void page_changed(tf_page *p) { p->need_style = 1; }

void page_layout_now(tf_page *p)
{
    if (!p->doc) return;
    if (p->styling) restyle_step(p, 0);                /* what a script or a click is about to see: all of it now */
    if (!p->styled_once) return;
    if (p->need_style || p->need_sheets || p->doc->changed) restyle(p);
    if (p->need_layout || p->laid_w != p->vw) relayout(p);
}

/* ------------------------------------------------------------------ the page */

static tf_page *page_new(const tf_host *host)
{
    tf_page *p = calloc(1, sizeof(*p));
    if (!p) return 0;
    p->host = *host;
    p->vw = 900;
    p->vh = 600;
    p->scroll_req = -1;
    p->loading = 1;
    p->style_rate = 20;
    return p;
}

/* a document is here: its sheets, scripts and pictures are found, and its scripts get an engine */
static void doc_ready(tf_page *p)
{
    p->phase = 1;
    int needs_js = 0;
    for (tf_node *n = p->doc->root; n && !needs_js; n = dom_next(n, p->doc->root)) {
        if (n->type != TF_ELEM) continue;
        if (dom_is(n, "script")) needs_js = 1;
        for (int i = 0; i < n->nattrs && !needs_js; i++) {
            const char *nm = lwc_string_data(n->attrs[i].name);
            if (nm[0] == 'o' && nm[1] == 'n') needs_js = 1;
            if (n->attrs[i].value && istarts(n->attrs[i].value, "javascript:")) needs_js = 1;
        }
    }
    if (needs_js) p->js = tfjs_new(p);
    scan(p, p->doc->root, 1);
    p->need_sheets = 1;
    p->need_style = 1;
}

static char *header_charset(webfetch_t *f, char *out, int cap)
{
    const char *h = (const char *)f->raw;
    int hl = (int)f->head_len;
    out[0] = 0;
    for (int i = 0; i + 13 < hl; i++) {
        if (i && h[i - 1] != '\n') continue;
        if (!istarts(h + i, "content-type:")) continue;
        for (int k = i; k < hl && h[k] != '\n'; k++)
            if (istarts(h + k, "charset=")) {
                k += 8;
                if (h[k] == '"') k++;
                int o = 0;
                while (k < hl && h[k] != '"' && h[k] != ';' && h[k] != '\r' && h[k] != '\n' && h[k] != ' ' && o < cap - 1) out[o++] = h[k++];
                out[o] = 0;
                return out;
            }
    }
    return out;
}

/* a page with no charset given anywhere: UTF-8 if its bytes are, else Windows-1252 (what old pages are) */
static const char *sniff_charset(const uint8_t *b, uint32_t n)
{
    uint32_t lim = n < 4096 ? n : 4096;
    for (uint32_t i = 0; i + 8 < lim; i++) if (istarts((const char *)b + i, "charset")) return 0;     /* the page says it */
    for (uint32_t i = 0; i < n && i < 65536; ) {
        uint8_t c = b[i];
        int k = c < 0x80 ? 0 : c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1;
        if (k < 0) return "windows-1252";
        for (int j = 1; j <= k; j++) if (i + (uint32_t)j >= n || (b[i + j] & 0xC0) != 0x80) return i + (uint32_t)j >= n ? "UTF-8" : "windows-1252";
        i += (uint32_t)k + 1;
    }
    return "UTF-8";
}

static void html_escape_into(char **buf, int *len, int *cap, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        const char *rep = s[i] == '<' ? "&lt;" : s[i] == '&' ? "&amp;" : s[i] == '>' ? "&gt;" : s[i] == '"' ? "&quot;" : 0;
        int rl = rep ? (int)strlen(rep) : 1;
        if (*len + rl + 1 > *cap) { int nc = (*cap + rl + 1) * 2; char *nb = realloc(*buf, (size_t)nc); if (!nb) return; *buf = nb; *cap = nc; }
        if (rep) { memcpy(*buf + *len, rep, (size_t)rl); *len += rl; } else (*buf)[(*len)++] = s[i];
        (*buf)[*len] = 0;
    }
}

static void html_raw_into(char **buf, int *len, int *cap, const char *s)
{
    int n = (int)strlen(s);
    if (*len + n + 1 > *cap) { int nc = (*len + n + 1) * 2; char *nb = realloc(*buf, (size_t)nc); if (!nb) return; *buf = nb; *cap = nc; }
    memcpy(*buf + *len, s, (size_t)n + 1);
    *len += n;
}

static void show_error(tf_page *p, const char *head, const char *msg)
{
    char *b = 0;
    int len = 0, cap = 0;
    html_raw_into(&b, &len, &cap, "<!doctype html><html><head><title>");
    html_escape_into(&b, &len, &cap, head, (int)strlen(head));
    html_raw_into(&b, &len, &cap, "</title><style>body{font-family:sans-serif;margin:48px;color:#1f2937}h1{font-size:26px;margin-bottom:8px}"
                                  "p{color:#4b5563}.u{color:#9ca3af;font-size:13px}</style></head><body><h1>");
    html_escape_into(&b, &len, &cap, head, (int)strlen(head));
    html_raw_into(&b, &len, &cap, "</h1><p>");
    html_escape_into(&b, &len, &cap, msg, (int)strlen(msg));
    html_raw_into(&b, &len, &cap, "</p><p class=u>");
    html_escape_into(&b, &len, &cap, p->url, (int)strlen(p->url));
    html_raw_into(&b, &len, &cap, "</p></body></html>");
    if (!b) { p->phase = 2; return; }
    if (p->doc) { if (p->js) { tfjs_free(p->js); p->js = 0; } dom_free(p->doc); p->doc = 0; }
    p->doc = dom_parse(b, (uint32_t)len, "UTF-8");
    free(b);
    if (p->doc) doc_ready(p);
    else page_console(p, 2, "could not even parse the error page (out of memory?)");
    p->phase = p->doc ? 1 : 2;
}

/* Content-Disposition: 1 when it says attachment; its filename (or filename*=UTF-8''...) into name */
static int disposition(webfetch_t *f, char *name, int cap)
{
    const char *h = (const char *)f->raw;
    int hl = (int)f->head_len, attach = 0;
    for (int i = 0; i + 20 < hl; i++) {
        if (i && h[i - 1] != '\n') continue;
        if (!istarts(h + i, "content-disposition:")) continue;
        int e = i;
        while (e < hl && h[e] != '\n') e++;
        for (int k = i + 20; k < e; k++) {
            if (istarts(h + k, "attachment")) attach = 1;
            int star = istarts(h + k, "filename*="), plain = istarts(h + k, "filename=");
            if (!star && !plain) continue;
            k += star ? 10 : 9;
            if (star) { while (k < e && h[k] != '\'') k++; while (k < e && h[k] == '\'') k++; }   /* UTF-8''name */
            if (k < e && h[k] == '"') k++;
            int o = 0;
            while (k < e && h[k] != '"' && h[k] != ';' && h[k] != '\r' && o < cap - 1) {
                if (h[k] == '%' && k + 2 < e && hexv(h[k + 1]) >= 0 && hexv(h[k + 2]) >= 0) { name[o++] = (char)(hexv(h[k + 1]) * 16 + hexv(h[k + 2])); k += 3; }
                else name[o++] = h[k++];
            }
            name[o] = 0;
            if (star) break;                              /* (the UTF-8 one wins) */
        }
    }
    return attach;
}

/* a name to save url under: the last part of its path, %-escapes decoded ("download" if there is none) */
static void name_from_url(const char *url, char *name, int cap)
{
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *path = strchr(s, '/');
    const char *last = 0;
    for (const char *c = path; c && *c && *c != '?' && *c != '#'; c++) if (*c == '/') last = c + 1;
    int o = 0;
    for (const char *c = last; c && *c && *c != '?' && *c != '#' && o < cap - 1; c++) {
        if (*c == '%' && hexv(c[1]) >= 0 && hexv(c[2]) >= 0) { name[o++] = (char)(hexv(c[1]) * 16 + hexv(c[2])); c += 2; }
        else name[o++] = *c;
    }
    name[o] = 0;
    if (!o) { strncpy(name, "download", (size_t)cap - 1); name[cap - 1] = 0; }
}

/* the answer is a file to save: kept for the window to take (page_take_download) */
static void make_download(tf_page *p, webfetch_t *f, const char *given)
{
    char name[128] = "";
    disposition(f, name, sizeof(name));
    if (given && *given) { strncpy(name, given, sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
    if (!name[0]) name_from_url(f->final_url[0] ? f->final_url : f->url, name, sizeof(name));
    for (char *c = name; *c; c++) if (*c == '/' || *c == '\\' || *c == ':' || (unsigned char)*c < 32) *c = '_';
    if (name[0] == '.') name[0] = '_';
    strcpy(p->dl_name, name);
    p->dl_raw = f->raw;
    p->dl_data = f->body;
    p->dl_len = f->body_len;
    f->raw = 0;                                           /* (it is the window's now) */
    f->body = 0;
    p->has_download = 1;
    p->phase = 1;
}

static void doc_fetched(tf_page *p)
{
    webfetch_t *f = p->docfetch;
    p->docfetch = 0;
    if (f->state != WEBFETCH_DONE) {
        show_error(p, "Can't open the page", f->error[0] ? f->error : "The page could not be fetched.");
        webfetch_free(f);
        return;
    }
#ifdef TFIRE_DEBUG
    {
        char m[200];
        snprintf(m, sizeof(m), "page: state %d status %d type %s, %u bytes", f->state, f->status, f->ctype, (unsigned)f->body_len);
        page_console(p, 0, m);
    }
#endif
    if (f->final_url[0]) { strncpy(p->url, f->final_url, URL_CAP - 1); p->url[URL_CAP - 1] = 0; p->has_url_change = 1; }
    const char *ct = f->ctype;
    {
        char dn[8];
        int shows = !ct[0] || starts(ct, "text/") || starts(ct, "application/xhtml") || starts(ct, "application/json") ||
                    starts(ct, "application/javascript") || starts(ct, "application/xml") || starts(ct, "image/");
        if (f->status < 400 && (p->req.download || disposition(f, dn, sizeof(dn)) || !shows)) {
            make_download(p, f, p->req.download ? p->req.dl_name : 0);
            webfetch_free(f);
            return;
        }
    }
    if (!ct[0] || starts(ct, "text/html") || starts(ct, "application/xhtml")) {
        char cs[40];
        header_charset(f, cs, sizeof(cs));
        const char *charset = cs[0] ? cs : sniff_charset(f->body, f->body_len);
        p->doc = dom_parse((const char *)f->body, f->body_len, charset);
        if (!p->doc) show_error(p, "Can't show the page", "tFire ran out of memory reading it.");
        else doc_ready(p);
    } else if (starts(ct, "text/") || starts(ct, "application/json") || starts(ct, "application/javascript") || starts(ct, "application/xml")) {
        char *b = 0;
        int len = 0, cap = 0;
        html_raw_into(&b, &len, &cap, "<!doctype html><pre style=\"white-space:pre-wrap\">");
        html_escape_into(&b, &len, &cap, (const char *)f->body, (int)f->body_len);
        if (b) { p->doc = dom_parse(b, (uint32_t)len, "UTF-8"); free(b); }
        if (p->doc) doc_ready(p); else p->phase = 2;
    } else if (starts(ct, "image/")) {
        char *b = malloc(URL_CAP * 2);
        if (b) {
            snprintf(b, URL_CAP * 2, "<!doctype html><body style=\"margin:0;background:#1f1f23;text-align:center\"><img src=\"%s\" style=\"margin-top:20px\"></body>", p->url);
            p->doc = dom_parse(b, (uint32_t)strlen(b), "UTF-8");
            free(b);
        }
        if (p->doc) {
            doc_ready(p);
            /* the picture is here already */
            image_t *im = &p->imgs[p->nimgs++];
            memset(im, 0, sizeof(*im));
            im->url = strdup(p->url);
            image_decode(p, im, f->body, f->body_len);
        } else p->phase = 2;
    } else {
        char m[200];
        snprintf(m, sizeof(m), "This is a file of type %s, which tFire cannot show.", ct);
        show_error(p, "Can't show this file", m);
    }
    if (p->doc && f->status >= 400 && !p->doc->root->first) show_error(p, "The server said no", "The server answered with an error.");
    webfetch_free(f);
}

typedef struct { tf_page *p; const tf_request *req; const char *html; int len; } open_args;

static void open_on_stack(void *a)
{
    open_args *o = a;
    tf_page *p = o->p;
    current = p;
    if (o->html) {
        p->doc = dom_parse(o->html, (uint32_t)o->len, "UTF-8");
        if (p->doc) doc_ready(p); else p->phase = 2;
    } else {
        const tf_request *r = o->req;
        char hdr[160] = "";
        if (r->body && r->ctype[0]) snprintf(hdr, sizeof(hdr), "Content-Type: %s\r\n", r->ctype);
        p->docfetch = webfetch_request(r->url, r->method[0] ? r->method : 0, hdr[0] ? hdr : 0, r->body, r->body_len);
#ifdef TFIRE_DEBUG
        {
            uint32_t tk, uk;
            memory_get_usage(&tk, &uk);
            char m[URL_CAP + 80];
            snprintf(m, sizeof(m), "open %s: fetch %s; kernel heap %u/%u KB", r->url, p->docfetch ? "started" : "NOT started", (unsigned)uk, (unsigned)tk);
            page_console(p, 0, m);
        }
#endif
        if (!p->docfetch) show_error(p, "Can't open the page", "tOS could not start fetching it.");
    }
    current = 0;
}

tf_page *page_open(const tf_request *req, const tf_host *host)
{
    tf_page *p = page_new(host);
    if (!p) { serial_write("[tfire] page_open: no memory for the page\n"); return 0; }
    p->req = *req;
    p->req.body = 0;
    strncpy(p->url, req->url, URL_CAP - 1);
    open_args a = { p, req, 0, 0 };
    run_big(open_on_stack, &a);
    return p;
}

tf_page *page_open_html(const char *url, const char *html, int len, const tf_host *host)
{
    tf_page *p = page_new(host);
    if (!p) return 0;
    strncpy(p->url, url, URL_CAP - 1);
    strncpy(p->req.url, url, URL_CAP - 1);
    open_args a = { p, 0, html, len };
    run_big(open_on_stack, &a);
    return p;
}

static void close_on_stack(void *a)
{
    tf_page *p = a;
    if (p->docfetch) webfetch_free(p->docfetch);
    if (p->fetch) webfetch_free(p->fetch);
    if (p->dl_raw) free(p->dl_raw);                    /* a download nobody took (tf_free gives kernel blocks back) */
    if (p->js) tfjs_free(p->js);
    anim_free(p->anims);
    style_free(p->styler);
    layout_clear(&p->L);
    if (p->doc) dom_free(p->doc);
    for (int i = 0; i < p->ncss; i++) { free(p->css[i].url); free(p->css[i].css); }
    free(p->css);
    for (int i = 0; i < p->nscripts; i++) { free(p->scripts[i].url); free(p->scripts[i].src); }
    free(p->scripts);
    for (int i = 0; i < p->nimgs; i++) { free(p->imgs[i].url); free(p->imgs[i].pic.rgba); }
    for (int i = 0; i < p->nxhrs; i++) { free(p->xhrs[i].method); free(p->xhrs[i].url); free(p->xhrs[i].headers); free(p->xhrs[i].body); }
    free(p->xhrs);
    for (int i = 0; i < p->nmods; i++) free(p->mods[i].url);
    for (int i = 0; i < p->ncons; i++) free(p->console[i]);
    free(p->nav.body);
    free(p);
}

void page_close(tf_page *p)
{
    if (!p) return;
    if (current == p) current = 0;
    run_big(close_on_stack, p);
}

/* ------------------------------------------------------------------ ticking */

/* animationstart, transitionend and the rest, to the page's scripts */
static void anim_events(tf_page *p)
{
    tf_node *n;
    char type[24], name[64];
    int el, id;
    while (anim_take_event(p->anims, &n, type, sizeof(type), name, sizeof(name), &el, &id)) {
        if (!p->js || !n || !dom_connected(p->doc, n)) continue;
        tfjs_event(p->js, n, type, id, 0, el, name);
    }
}

int page_anim_add(tf_page *p, tf_node *n, const char *frames, int len, int dur, int delay, const char *easing, int iter, int dir, int fill)
{
    if (!p->anims) p->anims = anim_new();
    page_layout_now(p);
    int id = anim_js_add(p->anims, n, frames, len, dur, delay, easing, iter, dir, fill, uihost_ms());
    p->anim_moving = 1;
    p->last_frame = 0;
    return id;
}

void page_anim_ctl(tf_page *p, int id, int op, int value)
{
    anim_js_ctl(p->anims, id, op, value, uihost_ms());
    p->anim_moving = 1;
    p->last_frame = 0;
}

int page_anim_info(tf_page *p, int id, int *current) { return anim_js_info(p->anims, id, current, uihost_ms()); }

typedef struct { tf_page *p; int w, h, changed; } tick_args;

static void tick_on_stack(void *a)
{
    tick_args *t = a;
    tf_page *p = t->p;
    current = p;
    if (t->w > 0 && t->w != p->vw) {
        if (p->styling) restyle_step(p, 0);
        p->vw = t->w; p->need_layout = 1;
        if (p->js && p->dcl_fired) tfjs_window_event(p->js, "resize", 0);
        p->need_style = 1;
    }
    if (t->h > 0) p->vh = t->h;

    if (p->docfetch) {
        int st = uihost_fetch_poll(p->docfetch);
        if (st != 0) { doc_fetched(p); t->changed = 1; }
    }
    if (p->phase != 1 || !p->doc) { p->loading = p->docfetch != 0; current = 0; return; }

    /* styling under way: another slice of it, and nothing else this tick (scripts and fetches wait: they could
     * change the tree in the middle of it) */
    if (p->styling) {
        if (restyle_step(p, 40)) { relayout(p); t->changed = 1; }
        p->loading = 1;
        current = 0;
        return;
    }

    /* the one fetch: finished? the next one */
    for (int k = 0; k < 4; k++) {
        if (p->fetch) {
            if (uihost_fetch_poll(p->fetch) == 0) break;
            fetch_finish(p);
            t->changed = 1;
        }
        if (!fetch_next(p)) break;
    }

    /* style once the sheets are in (or have failed); the first time, before any script runs */
    if (!css_pending(p) && (p->need_sheets || !p->styled_once)) {
        rebuild_sheets(p);
        restyle_begin(p);
        if (restyle_step(p, 40)) t->changed = 1;
        else { p->loading = 1; current = 0; return; }
    }

    if (p->styled_once) {
        int ran = scripts_step(p);
        if (ran) t->changed = 1;
        if (!p->dcl_fired && !scripts_pending(p) && !css_pending(p)) {
            p->dcl_fired = 1;
            if (p->js) tfjs_window_event(p->js, "DOMContentLoaded", 1);
        }
        if (p->dcl_fired && !p->load_fired) {
            p->load_fired = 1;
            if (p->js) { tfjs_window_event(p->js, "load", 0); tfjs_window_event(p->js, "pageshow", 0); }
            /* the address's #fragment */
            const char *h = strchr(p->url, '#');
            if (h && h[1]) {
                page_layout_now(p);
                tf_node *t2 = dom_by_id(p->doc, h + 1);
                if (t2 && t2->laid) p->scroll_req = t2->ly;
            }
        }
        if (p->js) {
            uint32_t now = uihost_ms();
            if (p->next_timer && (int32_t)(now - p->next_timer) >= 0) {
                p->next_timer = 0;
                int next = tfjs_tick(p->js);
                if (next >= 0) p->next_timer = now + (uint32_t)(next < 1 ? 1 : next);
            } else if (!p->next_timer) {
                int next = tfjs_tick(p->js);
                if (next >= 0) p->next_timer = now + (uint32_t)(next < 1 ? 1 : next);
            }
        }
        if (p->doc->changed) p->need_style = 1;
        if ((p->need_style || p->need_sheets) && !css_pending(p) && (p->style_soon || uihost_ms() - p->last_style >= RESTYLE_MS)) {
            p->style_soon = 0;
            restyle_begin(p);
            if (!restyle_step(p, 40)) { p->loading = 1; current = 0; return; }
            t->changed = 1;
        }
        /* what moves: a frame, as often as layout lets (at most 60 a second) */
        if (p->anim_moving && p->anims) {
            uint32_t now = uihost_ms(), gap = p->layout_ms * 2;
            if (gap < 16) gap = 16;
            if (gap > 250) gap = 250;
            if (now - p->last_frame >= gap) {
                p->last_frame = now;
                int r = anim_apply(p->anims, now);
                p->anim_moving = (r & 2) != 0;
                if (r & 1) p->layout_soon = 1;
            }
        }
        /* pictures arriving one after another: laid out again at most every half second (a width change, new
         * styles, a frame: now) */
        if (p->laid_w != p->vw || (p->need_layout && p->layout_soon) || (p->need_layout && uihost_ms() - p->last_layout >= 500) ||
            (p->layout_soon && p->anims)) { relayout(p); t->changed = 1; }
        anim_events(p);
    }
    int waiting = p->fetch != 0 || css_pending(p) || scripts_pending(p);
    for (int i = 0; i < p->nimgs && !waiting; i++) if (p->imgs[i].state == S_WAIT || p->imgs[i].state == S_FETCHING) waiting = 1;
    p->loading = waiting || !p->load_fired;
    current = 0;
}

int page_tick(tf_page *p, int w, int h)
{
    tick_args t = { p, w, h, 0 };
    run_big(tick_on_stack, &t);
    return t.changed;
}

int page_loading(tf_page *p) { return p->loading || p->docfetch != 0; }
int page_fetched(tf_page *p) { return p->phase != 0; }

int page_take_download(tf_page *p, char *name, int cap, void **raw, const uint8_t **data, uint32_t *len)
{
    if (!p->has_download) return 0;
    p->has_download = 0;
    strncpy(name, p->dl_name, (size_t)cap - 1);
    name[cap - 1] = 0;
    *raw = p->dl_raw;
    *data = p->dl_data;
    *len = p->dl_len;
    p->dl_raw = 0;
    return 1;
}
const char *page_url(tf_page *p) { return p->url; }
const char *page_title(tf_page *p) { return p->title; }
tf_layout *page_layout(tf_page *p) { return &p->L; }
tf_doc *page_doc(tf_page *p) { return p->doc; }
tf_node *page_focus(tf_page *p) { return p->focus; }
int page_caret_on(tf_page *p) { return ((uihost_ms() - p->caret_t0) / 530) % 2 == 0; }
int page_scroll_y(tf_page *p) { return p->scroll_y; }
int page_viewport_w(tf_page *p) { return p->vw; }
int page_viewport_h(tf_page *p) { return p->vh; }
void page_scroll_to(tf_page *p, int y) { p->scroll_req = y < 0 ? 0 : y; p->scroll_y = p->scroll_req; }

int page_take_scroll(tf_page *p) { int y = p->scroll_req; p->scroll_req = -1; return y; }

static void scrolled_ev(void *x)
{
    tf_page *p = x;
    current = p;
    if (p->styling) { current = 0; return; }       /* (told again on the next scroll) */
    tfjs_window_event(p->js, "scroll", 1);
    current = 0;
}

void page_scrolled(tf_page *p, int y)
{
    if (y == p->scroll_y) return;
    p->scroll_y = y;
    if (p->js && p->dcl_fired) run_big(scrolled_ev, p);
}

int page_take_navigation(tf_page *p, tf_request *out)
{
    if (!p->has_nav) return 0;
    *out = p->nav;
    p->nav.body = 0;
    p->has_nav = 0;
    return 1;
}

int page_take_url_change(tf_page *p, char *out, int cap)
{
    if (!p->has_url_change) return 0;
    p->has_url_change = 0;
    strncpy(out, p->url, (size_t)cap - 1);
    out[cap - 1] = 0;
    return 1;
}

void page_navigate(tf_page *p, const char *url, int replace)
{
    free(p->nav.body);
    memset(&p->nav, 0, sizeof(p->nav));
    strncpy(p->nav.url, url, URL_CAP - 1);
    p->nav.replace = replace;
    p->has_nav = 1;
}

void page_set_url(tf_page *p, const char *url, int replace)
{
    (void)replace;
    strncpy(p->url, url, URL_CAP - 1);
    p->url[URL_CAP - 1] = 0;
    p->has_url_change = 1;
}

void page_reload(tf_page *p) { page_navigate(p, p->url, 1); }

void page_alert(tf_page *p, const char *msg)
{
    page_console(p, 1, msg);
    if (p->host.alert) p->host.alert(p->host.ctx, msg);
}

/* ------------------------------------------------------------------ forms */

typedef struct { char *s; int len, cap; } buf_t;

static void bput(buf_t *b, const char *s, int n)
{
    if (b->len + n + 1 > b->cap) {
        int nc = (b->cap ? b->cap * 2 : 256);
        while (nc < b->len + n + 1) nc *= 2;
        char *ns = realloc(b->s, (size_t)nc);
        if (!ns) return;
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->len, s, (size_t)n);
    b->len += n;
    b->s[b->len] = 0;
}

static void benc(buf_t *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '*') bput(b, (const char *)&c, 1);
        else if (c == ' ') bput(b, "+", 1);
        else if (c == '\n') bput(b, "%0D%0A", 6);
        else if (c == '\r') continue;
        else { char e[3] = { '%', "0123456789ABCDEF"[c >> 4], "0123456789ABCDEF"[c & 15] }; bput(b, e, 3); }
    }
}

static int is_field(tf_node *n)
{
    return dom_is(n, "input") || dom_is(n, "select") || dom_is(n, "textarea") || dom_is(n, "button");
}

static tf_node *form_of(tf_page *p, tf_node *n)
{
    const char *fid = dom_attr(n, "form");
    if (fid) { tf_node *f = dom_by_id(p->doc, fid); if (f && dom_is(f, "form")) return f; }
    for (tf_node *a = n->parent; a; a = a->parent) if (dom_is(a, "form")) return a;
    return 0;
}

static int checked(tf_node *n) { return n->checked == 1 || (n->checked == 0 && dom_attr(n, "checked")); }

static char *field_value(tf_node *n)
{
    if (n->value) return strdup(n->value);
    if (dom_is(n, "textarea")) return dom_text_content(n);
    if (dom_is(n, "select")) {
        tf_node *sel = 0, *first = 0;
        for (tf_node *o = n; o; o = dom_next(o, n))
            if (dom_is(o, "option")) { if (!first) first = o; if (o->checked == 1 || (o->checked == 0 && dom_attr(o, "selected"))) sel = o; }
        if (!sel) sel = first;
        if (!sel) return strdup("");
        const char *v = dom_attr(sel, "value");
        return v ? strdup(v) : dom_text_content(sel);
    }
    const char *v = dom_attr(n, "value");
    const char *t = dom_attr(n, "type");
    if (!v && t && (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio"))) v = "on";
    return strdup(v ? v : "");
}

void page_submit(tf_page *p, tf_node *form, tf_node *submitter)
{
    if (!form || !p->doc) return;
    const char *action = submitter && dom_attr(submitter, "formaction") ? dom_attr(submitter, "formaction") : dom_attr(form, "action");
    const char *method = submitter && dom_attr(submitter, "formmethod") ? dom_attr(submitter, "formmethod") : dom_attr(form, "method");
    const char *enc = dom_attr(form, "enctype");
    int post = method && !strcasecmp(method, "post");
    int multipart = post && enc && istarts(enc, "multipart/form-data");
    char target[URL_CAP];
    if (!action || !*action) strcpy(target, p->url);
    else if (istarts(action, "javascript:")) { if (p->js) tfjs_run(p->js, action + 11, (int)strlen(action + 11), p->url, 0, 0); return; }
    else if (!page_resolve(p->url, action, target, sizeof(target))) return;
    const char *fid = dom_attr(form, "id");
    buf_t b = { 0, 0, 0 };
    const char *boundary = "----tFireFormBoundary7MA4YWxkTrZu0gW";
    for (tf_node *n = p->doc->root; n; n = dom_next(n, p->doc->root)) {
        if (!is_field(n)) continue;
        int mine = dom_contains(form, n);
        if (!mine && fid) { const char *f = dom_attr(n, "form"); mine = f && !strcmp(f, fid); }
        if (!mine) continue;
        const char *name = dom_attr(n, "name");
        if (!name || !*name || dom_attr(n, "disabled")) continue;
        const char *type = dom_attr(n, "type");
        int button = dom_is(n, "button") || (dom_is(n, "input") && type && (!strcasecmp(type, "submit") || !strcasecmp(type, "image") || !strcasecmp(type, "reset") || !strcasecmp(type, "button")));
        if (button && n != submitter) continue;
        if (dom_is(n, "input") && type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) && !checked(n)) continue;
        if (dom_is(n, "input") && type && !strcasecmp(type, "file")) continue;
        char *v = button ? strdup(dom_attr(n, "value") ? dom_attr(n, "value") : "") : field_value(n);
        if (!v) continue;
        if (multipart) {
            bput(&b, "--", 2); bput(&b, boundary, (int)strlen(boundary));
            bput(&b, "\r\nContent-Disposition: form-data; name=\"", 39); bput(&b, name, (int)strlen(name));
            bput(&b, "\"\r\n\r\n", 5); bput(&b, v, (int)strlen(v)); bput(&b, "\r\n", 2);
        } else {
            if (b.len) bput(&b, "&", 1);
            benc(&b, name);
            bput(&b, "=", 1);
            benc(&b, v);
        }
        free(v);
    }
    if (multipart) { bput(&b, "--", 2); bput(&b, boundary, (int)strlen(boundary)); bput(&b, "--\r\n", 4); }
    free(p->nav.body);
    memset(&p->nav, 0, sizeof(p->nav));
    if (post) {
        strcpy(p->nav.url, target);
        strcpy(p->nav.method, "POST");
        if (multipart) snprintf(p->nav.ctype, sizeof(p->nav.ctype), "multipart/form-data; boundary=%s", boundary);
        else strcpy(p->nav.ctype, "application/x-www-form-urlencoded");
        p->nav.body = b.s ? b.s : strdup("");
        p->nav.body_len = b.len;
    } else {
        char *q = strchr(target, '?');
        char *h = strchr(target, '#');
        if (h) *h = 0;
        if (q) *q = 0;
        snprintf(p->nav.url, sizeof(p->nav.url), "%s?%s", target, b.s ? b.s : "");
        free(b.s);
    }
    p->has_nav = 1;
}

/* ------------------------------------------------------------------ the person: clicks and keys */

static int field_kind(tf_node *n)                 /* 1 text field, 2 check box, 3 radio, 4 button, 5 select, 0 none */
{
    if (dom_is(n, "textarea")) return 1;
    if (dom_is(n, "select")) return 5;
    if (dom_is(n, "button")) return 4;
    if (!dom_is(n, "input")) return 0;
    const char *t = dom_attr(n, "type");
    if (!t) return 1;
    if (!strcasecmp(t, "checkbox")) return 2;
    if (!strcasecmp(t, "radio")) return 3;
    if (!strcasecmp(t, "submit") || !strcasecmp(t, "button") || !strcasecmp(t, "reset") || !strcasecmp(t, "image")) return 4;
    if (!strcasecmp(t, "hidden") || !strcasecmp(t, "file") || !strcasecmp(t, "range") || !strcasecmp(t, "color")) return 0;
    return 1;
}

static void ev(tf_page *p, tf_node *n, const char *type) { if (p->js && n) tfjs_event(p->js, n, type, 0, 0, 0, 0); }

void page_focus_node(tf_page *p, tf_node *n)
{
    if (n == p->focus) return;
    tf_node *old = p->focus;
    p->focus = n;
    p->caret_t0 = uihost_ms();
    if (old) { ev(p, old, "blur"); ev(p, old, "focusout"); if (old->value && field_kind(old) == 1) ev(p, old, "change"); }
    if (n) { ev(p, n, "focus"); ev(p, n, "focusin"); }
    p->need_layout = 1;
}

/* the submit button a form would use for Enter */
static tf_node *default_button(tf_page *p, tf_node *form)
{
    for (tf_node *n = form; n; n = dom_next(n, form))
        if (field_kind(n) == 4) {
            const char *t = dom_attr(n, "type");
            if (dom_is(n, "button") ? (!t || !strcasecmp(t, "submit")) : (t && (!strcasecmp(t, "submit") || !strcasecmp(t, "image")))) return n;
        }
    (void)p;
    return 0;
}

static void submit_with_event(tf_page *p, tf_node *form, tf_node *submitter)
{
    if (p->js && tfjs_event(p->js, form, "submit", 0, 0, 0, 0)) return;
    page_submit(p, form, submitter);
}

static void set_checked(tf_page *p, tf_node *n, int on)
{
    n->checked = on ? 1 : 2;
    if (on && field_kind(n) == 3) {
        const char *name = dom_attr(n, "name");
        if (name)
            for (tf_node *o = p->doc->root; o; o = dom_next(o, p->doc->root))
                if (o != n && field_kind(o) == 3) { const char *n2 = dom_attr(o, "name"); if (n2 && !strcmp(n2, name)) o->checked = 2; }
    }
    p->need_style = 1;
}

typedef struct { tf_page *p; int x, y, k, r; } input_args;

static void click_on_stack(void *a)
{
    input_args *in = a;
    tf_page *p = in->p;
    current = p;
    if (!p->doc || !p->styled_once) { current = 0; return; }
    page_layout_now(p);
    tf_node *t = layout_hit(&p->L, p->doc, in->x, in->y);
    if (!t) t = dom_find(p->doc->root, "body");
    if (!t) { current = 0; return; }
    if (t->type != TF_ELEM && t->parent) t = t->parent;
    /* a check box changes before its click event (and back if a listener prevents it) */
    tf_node *box = 0;
    int was = 0;
    for (tf_node *a2 = t; a2 && !box; a2 = a2->parent) {
        int fk = field_kind(a2);
        if ((fk == 2 || fk == 3) && !dom_attr(a2, "disabled")) { box = a2; was = checked(a2); set_checked(p, a2, fk == 2 ? !was : 1); }
        if (dom_is(a2, "label")) {
            const char *f = dom_attr(a2, "for");
            tf_node *c = f ? dom_by_id(p->doc, f) : 0;
            if (!c) for (tf_node *k = a2; k && !c; k = dom_next(k, a2)) if (field_kind(k)) c = k;
            if (c && (field_kind(c) == 2 || field_kind(c) == 3) && !dom_attr(c, "disabled")) { box = c; was = checked(c); set_checked(p, c, field_kind(c) == 2 ? !was : 1); }
            else if (c && field_kind(c) == 1) { page_focus_node(p, c); }
            break;
        }
    }
    int prevented = 0;
    if (p->js) {
        tfjs_event(p->js, t, "pointerdown", in->x, in->y, 0, 0);
        prevented = tfjs_event(p->js, t, "mousedown", in->x, in->y, 0, 0);
        tfjs_event(p->js, t, "pointerup", in->x, in->y, 0, 0);
        tfjs_event(p->js, t, "mouseup", in->x, in->y, 0, 0);
        prevented = tfjs_event(p->js, t, "click", in->x, in->y, 0, 0) || 0;
    }
    if (box) {
        if (prevented) set_checked(p, box, was);
        else if (checked(box) != was) { ev(p, box, "input"); ev(p, box, "change"); }
        current = 0;
        return;
    }
    if (prevented) { current = 0; return; }
    /* what the click means, from the element clicked outwards */
    for (tf_node *a2 = t; a2; a2 = a2->parent) {
        if (a2->type != TF_ELEM) continue;
        if ((dom_is(a2, "a") || dom_is(a2, "area")) && dom_attr(a2, "href")) {
            const char *href = dom_attr(a2, "href");
            if (istarts(href, "javascript:")) {
                if (p->js) {
                    /* (percent-escapes in the code are decoded, as the address it is) */
                    const char *code = href + 11;
                    char *dec = malloc(strlen(code) + 1);
                    if (dec) {
                        int o = 0;
                        for (const char *c = code; *c; c++) {
                            if (*c == '%' && hexv(c[1]) >= 0 && hexv(c[2]) >= 0) { dec[o++] = (char)(hexv(c[1]) * 16 + hexv(c[2])); c += 2; }
                            else dec[o++] = *c;
                        }
                        dec[o] = 0;
                        tfjs_run(p->js, dec, o, p->url, 0, 0);
                        free(dec);
                    }
                }
            } else if (href[0] == '#') {
                tf_node *tg = href[1] ? dom_by_id(p->doc, href + 1) : 0;
                char u[URL_CAP];
                strncpy(u, p->url, sizeof(u) - 1);
                u[sizeof(u) - 1] = 0;
                char *h = strchr(u, '#');
                if (h) *h = 0;
                if (strlen(u) + strlen(href) < sizeof(u)) strcat(u, href);
                page_set_url(p, u, 0);
                p->scroll_req = tg && tg->laid ? tg->ly : href[1] ? -1 : 0;
                if (p->js) tfjs_window_event(p->js, "hashchange", 0);
            } else if (istarts(href, "tfire:")) {
                page_navigate(p, href, 0);                  /* tFire's own pages (the window opens them) */
            } else {
                char u[URL_CAP];
                if (page_resolve(p->url, href, u, sizeof(u))) {
                    page_navigate(p, u, 0);
                    const char *dl = dom_attr(a2, "download");
                    if (dl) {                                 /* <a download>: saved, whatever it is */
                        p->nav.download = 1;
                        strncpy(p->nav.dl_name, dl, sizeof(p->nav.dl_name) - 1);
                    }
                }
            }
            break;
        }
        int fk = field_kind(a2);
        if (fk && dom_attr(a2, "disabled")) break;
        if (fk == 1) { page_focus_node(p, a2); current = 0; return; }
        if (fk == 4) {
            const char *type = dom_attr(a2, "type");
            tf_node *form = form_of(p, a2);
            if (form && (!type || !strcasecmp(type, "submit") || !strcasecmp(type, "image"))) submit_with_event(p, form, a2);
            else if (form && type && !strcasecmp(type, "reset")) {
                for (tf_node *n = form; n; n = dom_next(n, form)) { if (n->value) { free(n->value); n->value = 0; } n->checked = 0; }
                p->need_style = 1;
            }
            break;
        }
        if (fk == 5) {
            /* a list of choices: the next one */
            tf_node *opts[256];
            int no = 0, cur = 0;
            for (tf_node *o = a2; o && no < 256; o = dom_next(o, a2))
                if (dom_is(o, "option") && !dom_attr(o, "disabled")) { if (o->checked == 1 || (o->checked == 0 && dom_attr(o, "selected"))) cur = no; opts[no++] = o; }
            if (no > 1) {
                int nx = (cur + 1) % no;
                for (int i = 0; i < no; i++) { opts[i]->checked = i == nx ? 1 : 2; if (i == nx) dom_set_attr(p->doc, opts[i], "selected", ""); else dom_remove_attr(p->doc, opts[i], "selected"); }
                ev(p, a2, "input");
                ev(p, a2, "change");
                p->need_style = 1;
            }
            page_focus_node(p, a2);
            current = 0;
            return;
        }
        if (dom_is(a2, "summary") && a2->parent && dom_is(a2->parent, "details")) {
            if (dom_attr(a2->parent, "open")) dom_remove_attr(p->doc, a2->parent, "open"); else dom_set_attr(p->doc, a2->parent, "open", "");
            ev(p, a2->parent, "toggle");
            p->need_style = 1;
            break;
        }
    }
    if (p->focus && !dom_contains(t, p->focus) && t != p->focus) page_focus_node(p, 0);
    current = 0;
}

/* the pointer moved to (x, y) of the page (x < 0: it left): mouseover and mouseout, mouseenter and mouseleave down
 * the elements entered and left, and a restyle when a :hover rule cares */
static void hover_on_stack(void *a)
{
    input_args *in = a;
    tf_page *p = in->p;
    in->r = 0;
    if (!p->doc || !p->styled_once) return;
    current = p;
    tf_node *t = in->x < 0 ? 0 : layout_hit(&p->L, p->doc, in->x, in->y);
    if (t && t->type != TF_ELEM) t = t->parent;
    if (t && t->type != TF_ELEM) t = 0;
    tf_node *old = p->hover;
    if (old && !dom_connected(p->doc, old)) old = 0;
    if (t == old) { if (t && p->js) tfjs_event(p->js, t, "mousemove", in->x, in->y, 0, 0); current = 0; return; }
    p->hover = t;
    if (p->js) {
        if (old) {
            tfjs_event(p->js, old, "pointerout", in->x, in->y, 0, 0);
            tfjs_event(p->js, old, "mouseout", in->x, in->y, 0, 0);
            for (tf_node *n = old; n && n->type == TF_ELEM; n = n->parent) {
                if (t && dom_contains(n, t)) break;
                tfjs_event(p->js, n, "mouseleave", in->x, in->y, 0, 0);
            }
        }
        if (t && dom_connected(p->doc, t)) {
            tfjs_event(p->js, t, "pointerover", in->x, in->y, 0, 0);
            tfjs_event(p->js, t, "mouseover", in->x, in->y, 0, 0);
            tf_node *chain[64];
            int nc = 0;
            for (tf_node *n = t; n && n->type == TF_ELEM && nc < 64; n = n->parent) {
                if (old && dom_contains(n, old)) break;
                chain[nc++] = n;
            }
            while (nc > 0) tfjs_event(p->js, chain[--nc], "mouseenter", in->x, in->y, 0, 0);
            tfjs_event(p->js, t, "mousemove", in->x, in->y, 0, 0);
        }
    }
    if (style_hover_matters(p->styler, old) || style_hover_matters(p->styler, t)) { p->need_style = 1; p->style_soon = 1; }
    in->r = 1;
    current = 0;
}

int page_hover(tf_page *p, int x, int y)
{
    input_args a = { p, x, y, 0, 0 };
    run_big(hover_on_stack, &a);
    return a.r;
}

/* the link under (x, y) of the page, or 0 */
const char *page_link_at(tf_page *p, int x, int y)
{
    if (!p->doc) return 0;
    tf_node *t = layout_hit(&p->L, p->doc, x, y);
    for (; t; t = t->parent) if (t->type == TF_ELEM && dom_is(t, "a") && dom_attr(t, "href")) return dom_attr(t, "href");
    return 0;
}

void page_click(tf_page *p, int x, int y)
{
    input_args a = { p, x, y, 0, 0 };
    run_big(click_on_stack, &a);
}

/* the next (or previous) text field after n, for Tab */
static tf_node *next_field(tf_page *p, tf_node *n)
{
    for (tf_node *k = n ? dom_next(n, p->doc->root) : p->doc->root; k; k = dom_next(k, p->doc->root))
        if (field_kind(k) && field_kind(k) != 4 && !dom_attr(k, "disabled") && k->laid) return k;
    return 0;
}

static void key_on_stack(void *a)
{
    input_args *in = a;
    tf_page *p = in->p;
    int k = in->k;
    current = p;
    if (p->styling) restyle_step(p, 0);                /* (scripts that hear the key may change the tree) */
    in->r = 0;
    if (!p->doc) { current = 0; return; }
    char name[16] = "";
    int code = k;
    if (k == '\n') { strcpy(name, "Enter"); code = 13; }
    else if (k == '\b') { strcpy(name, "Backspace"); code = 8; }
    else if (k == '\t') { strcpy(name, "Tab"); code = 9; }
    else if (k == UI_KEY_ESC) { strcpy(name, "Escape"); code = 27; }
    else if (k == UI_KEY_UP) { strcpy(name, "ArrowUp"); code = 38; }
    else if (k == UI_KEY_DOWN) { strcpy(name, "ArrowDown"); code = 40; }
    else if (k == UI_KEY_LEFT) { strcpy(name, "ArrowLeft"); code = 37; }
    else if (k == UI_KEY_RIGHT) { strcpy(name, "ArrowRight"); code = 39; }
    else if (k == UI_KEY_DEL) { strcpy(name, "Delete"); code = 46; }
    else if (k < 0x80) { name[0] = (char)k; name[1] = 0; code = k >= 'a' && k <= 'z' ? k - 32 : k; }
    else { name[0] = (char)(0xC0 | (k >> 6)); name[1] = (char)(0x80 | (k & 0x3F)); name[2] = 0; }
    tf_node *target = p->focus ? p->focus : dom_find(p->doc->root, "body");
    int prevented = 0;
    if (p->js && target) {
        prevented = tfjs_event(p->js, target, "keydown", 0, 0, code, name);
        if (!prevented && (k >= 32 || k == '\n') && k != UI_KEY_DEL) prevented = tfjs_event(p->js, target, "keypress", 0, 0, k == '\n' ? 13 : k, name);
    }
    tf_node *f = p->focus;
    if (f && !dom_connected(p->doc, f)) { p->focus = 0; f = 0; }
    if (prevented) {
        if (p->js && target) tfjs_event(p->js, target, "keyup", 0, 0, code, name);
        in->r = 1;
        current = 0;
        return;
    }
    if (f && field_kind(f) == 1 && !dom_attr(f, "readonly")) {
        in->r = 1;
        p->caret_t0 = uihost_ms();
        int textarea = dom_is(f, "textarea");
        if (k == '\n' && !textarea) {
            tf_node *form = form_of(p, f);
            if (form) {
                ev(p, f, "change");
                submit_with_event(p, form, default_button(p, form));
            }
        } else if (k == '\t') {
            page_focus_node(p, next_field(p, f));
        } else if (k == UI_KEY_ESC) {
            page_focus_node(p, 0);
        } else if (k == '\b' || k >= 32 || (k == '\n' && textarea)) {
            if (!f->value) {
                char *v = field_value(f);
                f->value = v ? v : strdup("");
            }
            int l = (int)strlen(f->value);
            if (k == '\b') {
                if (l) { while (l > 1 && ((unsigned char)f->value[l - 1] & 0xC0) == 0x80) l--; f->value[l - 1] = 0; }
            } else if (k != UI_KEY_UP && k != UI_KEY_DOWN && k != UI_KEY_LEFT && k != UI_KEY_RIGHT && k != UI_KEY_DEL) {
                const char *ml = dom_attr(f, "maxlength");
                if (!ml || atoi(ml) <= 0 || l < atoi(ml)) {
                    char *nv = realloc(f->value, (size_t)l + 4);
                    if (nv) {
                        f->value = nv;
                        int n = (int)strlen(name);
                        if (k == '\n') { nv[l] = '\n'; n = 1; } else memcpy(nv + l, name, (size_t)n);
                        nv[l + n] = 0;
                    }
                }
            }
            if (p->js) tfjs_event(p->js, f, "input", 0, 0, k == '\b' ? 8 : 0, k == '\b' ? "" : name);
            p->need_style = 1;
        }
    } else if (f && (k == '\n' || k == ' ') && field_kind(f) >= 2) {
        input_args c = { p, f->lx + 2, f->ly + 2, 0, 0 };
        click_on_stack(&c);
        in->r = 1;
    } else if (k == '\t') {
        page_focus_node(p, next_field(p, f));
        in->r = 1;
    }
    if (p->js && target) tfjs_event(p->js, target, "keyup", 0, 0, code, name);
    current = 0;
}

int page_key(tf_page *p, int k)
{
    input_args a = { p, 0, 0, k, 0 };
    run_big(key_on_stack, &a);
    return a.r;
}
