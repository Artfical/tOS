#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/*
 * tFire, the web browser (Project tFire, update 1).
 *
 * A page is fetched in the background (uihost_fetch), its HTML turned into runs of styled text with
 * paragraph breaks and links (parse), the runs broken into words placed on lines for the window's width
 * (layout), and the words on screen drawn (draw). No scripts, no style sheets, no pictures yet: the engine
 * of later updates comes from Firefox's own source; this is the browser around it.
 */

#define TB_H     46              /* toolbar */
#define URL_MAX  512
#define HIST_MAX 32
#define MARGIN   24
#define SEARCH   "https://html.duckduckgo.com/html/?q="
#define HOME     "tfire:home"

/* run styles */
#define ST_BOLD 1
#define ST_H1   2
#define ST_H2   4
#define ST_H3   8
#define ST_PRE  16
#define ST_DIM  32

typedef struct {
    uint32_t off, len;            /* text in the pool */
    uint8_t style;
    uint8_t brk;                  /* before it: 0 nothing, 1 a new line, 2 a new paragraph */
    uint8_t indent;               /* list depth */
    uint8_t bullet;               /* starts a list item */
    int16_t link;                 /* index into links, -1 = none */
    int16_t img;                  /* a picture (index into imgs) instead of text, -1 = none */
} run_t;

typedef struct {
    int16_t x, w;
    int32_t y;                    /* top of the line, from the top of the page */
    uint32_t off;
    uint16_t len;
    uint8_t style;
    int16_t link;
    int16_t img;                  /* a picture, -1 = text */
    int16_t h;                    /* a picture's height */
} box_t;

/* a picture on the page: fetched one after another once the page is there */
#define IMG_WAIT 0
#define IMG_LOADING 1
#define IMG_OK 2
#define IMG_FAILED 3
#define IMG_MAX 48
#define IMG_MAX_W 860
typedef struct {
    char *src;                    /* as the page wrote it */
    int aw, ah;                   /* the width and height the page gave (0 = not given) */
    int state;
    gfx_image_t im;               /* IMG_OK: the pixels, already the size to show */
} timg_t;

typedef struct {
    char url[URL_MAX];            /* the page shown (or being loaded) */
    char field[URL_MAX];          /* the address bar */
    int editing, select_all;
    char hist[HIST_MAX][URL_MAX];
    int nhist, hpos;
    void *fetch;
    uint32_t load_start;
    /* the page */
    char *pool; uint32_t pool_len, pool_cap;
    run_t *runs; int nruns, runs_cap;
    char **links; int nlinks, links_cap;
    char title[96];
    timg_t imgs[IMG_MAX]; int nimgs;
    void *img_fetch; int img_cur;
    /* laid out */
    box_t *boxes; int nboxes, boxes_cap;
    int laid_w, doc_h;
    int scroll;
    uint32_t blink;
} tf_t;

/* ------------------------------------------------------------------ the page's text */

static void imgs_free(tf_t *t)
{
    if (t->img_fetch) { uihost_fetch_free(t->img_fetch); t->img_fetch = 0; }
    for (int i = 0; i < t->nimgs; i++) { free(t->imgs[i].src); free((void *)t->imgs[i].im.rgba); }
    memset(t->imgs, 0, sizeof(t->imgs));
    t->nimgs = 0;
    t->img_cur = -1;
}

static void page_free(tf_t *t)
{
    imgs_free(t);
    for (int i = 0; i < t->nlinks; i++) free(t->links[i]);
    free(t->links); free(t->runs); free(t->pool); free(t->boxes);
    t->links = 0; t->runs = 0; t->pool = 0; t->boxes = 0;
    t->nlinks = t->links_cap = t->nruns = t->runs_cap = t->nboxes = t->boxes_cap = 0;
    t->pool_len = t->pool_cap = 0;
    t->title[0] = 0;
    t->laid_w = 0;
    t->doc_h = 0;
    t->scroll = 0;
}

static int grow(void **p, int *cap, int need, int size)
{
    if (need <= *cap) return 1;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *n = krealloc(*p, (size_t)nc * (size_t)size);
    if (!n) return 0;
    *p = n;
    *cap = nc;
    return 1;
}

/* what the builder is in the middle of */
typedef struct {
    tf_t *t;
    int style, link, indent;
    int pending_brk, bullet;
    int space;                    /* a space is owed before the next word */
    int pre;
    int run_open;
    int dd;                       /* inside a <dd>: one step further in */
} build_t;

static void put_bytes(build_t *b, const char *s, int n)
{
    tf_t *t = b->t;
    int cap = (int)t->pool_cap;
    if (!grow((void **)&t->pool, &cap, (int)t->pool_len + n + 1, 1)) return;
    t->pool_cap = (uint32_t)cap;
    if (!b->run_open || b->pending_brk || b->bullet) {
        if (!grow((void **)&t->runs, &t->runs_cap, t->nruns + 1, sizeof(run_t))) return;
        run_t *r = &t->runs[t->nruns++];
        r->off = t->pool_len; r->len = 0;
        r->style = (uint8_t)b->style; r->link = (int16_t)b->link;
        r->brk = (uint8_t)(t->nruns == 1 ? 0 : b->pending_brk);
        r->indent = (uint8_t)(b->indent + b->dd); r->bullet = (uint8_t)b->bullet;
        r->img = -1;
        b->pending_brk = 0; b->bullet = 0; b->run_open = 1;
    }
    memcpy(t->pool + t->pool_len, s, (size_t)n);
    t->pool_len += (uint32_t)n;
    t->runs[t->nruns - 1].len += (uint32_t)n;
}

/* the style changed: what follows is a run of its own */
static void restyle(build_t *b) { b->run_open = 0; }

static void brk(build_t *b, int kind)
{
    if (kind > b->pending_brk) b->pending_brk = kind;
    b->space = 0;
    b->run_open = 0;
}

static void put_char(build_t *b, uint32_t cp)
{
    char u[4];
    int n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    put_bytes(b, u, n);
}

/* text between tags: white space folds to single spaces unless inside <pre> */
static void text(build_t *b, uint32_t cp)
{
    if (b->pre) {
        if (cp == '\n') { brk(b, 1); return; }
        if (cp == '\r') return;
        if (cp == '\t') { put_bytes(b, "    ", 4); return; }
        put_char(b, cp);
        return;
    }
    if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == '\f') {
        if (b->run_open || b->t->nruns) b->space = 1;
        return;
    }
    if (b->space) {
        b->space = 0;
        if (!b->pending_brk && b->t->nruns) put_bytes(b, " ", 1);
    }
    put_char(b, cp == 0xA0 ? ' ' : cp);
}

static const struct { const char *name; uint16_t cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"deg", 0xB0}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"bull", 0x2022}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"larr", 0x2190}, {"rarr", 0x2192},
    {"times", 0xD7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"trade", 0x2122}, {"shy", 0},
};

/* &...; at s (s[0] == '&'): the character, and how many bytes it took (0 = not an entity) */
static int entity(const char *s, const char *end, uint32_t *cp)
{
    const char *p = s + 1;
    if (p < end && *p == '#') {
        p++;
        uint32_t v = 0;
        int hex = p < end && (*p == 'x' || *p == 'X');
        if (hex) p++;
        const char *d0 = p;
        while (p < end && p - d0 < 8) {
            char c = *p;
            if (c >= '0' && c <= '9') v = v * (hex ? 16 : 10) + (uint32_t)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
            else break;
            p++;
        }
        if (p == d0) return 0;
        if (p < end && *p == ';') p++;
        *cp = v && v < 0x110000 ? v : 0xFFFD;
        return (int)(p - s);
    }
    char name[10];
    int n = 0;
    while (p < end && n < 9 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) name[n++] = *p++;
    name[n] = 0;
    if (p >= end || *p != ';') return 0;
    for (unsigned i = 0; i < sizeof(entities) / sizeof(entities[0]); i++)
        if (!strcmp(name, entities[i].name)) { *cp = entities[i].cp; return (int)(p + 1 - s); }
    return 0;
}

/* one UTF-8 character (bad bytes are taken as Latin-1, which old pages often are) */
static uint32_t utf8(const char **ps, const char *end)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n && (const char *)s + n < end + 0) {
        uint32_t v = c & (0x3F >> n);
        int ok = 1;
        for (int i = 1; i <= n; i++) { if ((s[i] & 0xC0) != 0x80) { ok = 0; break; } v = (v << 6) | (s[i] & 0x3F); }
        if (ok) { *ps += n + 1; return v; }
    }
    *ps += 1;
    return c;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int tag_is(const char *tag, const char *name) { return !strcmp(tag, name); }

static int is_block(const char *t)
{
    static const char *const blocks[] = {
        "p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "ul", "ol", "dl", "dt", "dd",
        "table", "tr", "blockquote", "form", "figure", "figcaption", "address", "center", "fieldset", "details",
        "summary", "caption", "tbody", "thead", "tfoot", "menu", "hgroup", 0 };
    for (int i = 0; blocks[i]; i++) if (!strcmp(t, blocks[i])) return 1;
    return 0;
}

static int add_link(tf_t *t, const char *href, int n)
{
    if (t->nlinks >= 30000 || !grow((void **)&t->links, &t->links_cap, t->nlinks + 1, sizeof(char *))) return -1;
    char *s = malloc((size_t)n + 1);
    if (!s) return -1;
    int o = 0;
    const char *e = href + n;
    while (href < e) {                        /* "&amp;" -> "&" (the entities that matter in addresses) */
        uint32_t cp;
        int m = *href == '&' ? entity(href, e, &cp) : 0;
        if (m && cp > 0 && cp < 0x80) { s[o++] = (char)cp; href += m; }
        else s[o++] = *href++;
    }
    s[o] = 0;
    t->links[t->nlinks] = s;
    return t->nlinks++;
}

/* the value of attribute name in the tag text [a, e), or 0 (its length in *n) */
static const char *attr(const char *a, const char *e, const char *name, int *n)
{
    int nl = (int)strlen(name);
    for (const char *p = a; p + nl < e; p++) {
        if (p > a && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') continue;
        int k = 0;
        while (k < nl && lower(p[k]) == name[k]) k++;
        if (k < nl) continue;
        const char *q = p + nl;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        char quote = (q < e && (*q == '"' || *q == '\'')) ? *q++ : 0;
        const char *v = q;
        while (q < e && (quote ? *q != quote : (*q != ' ' && *q != '>' && *q != '\t' && *q != '\n'))) q++;
        *n = (int)(q - v);
        return v;
    }
    return 0;
}

/* an attribute's value as text: entities and UTF-8 decoded, at most max characters */
static void attr_text(build_t *b, const char *v, int n, int max)
{
    const char *e = v ? v + n : 0;
    for (int k = 0; v && v < e && k < max; k++) {
        uint32_t cp;
        int m = *v == '&' ? entity(v, e, &cp) : 0;
        if (m) v += m;
        else cp = utf8(&v, e);
        if (cp) text(b, cp);
    }
}

/* a picture: a run of its own, whose text is the picture's alt text, shown when it can't be */
static void put_image(build_t *b, int idx, const char *alt, int al)
{
    tf_t *t = b->t;
    uint32_t off = t->pool_len, n = 0;
    if (alt && al > 0) {
        if (al > 120) al = 120;
        int cap = (int)t->pool_cap;
        if (grow((void **)&t->pool, &cap, (int)t->pool_len + al + 3, 1)) {
            t->pool_cap = (uint32_t)cap;
            t->pool[t->pool_len++] = '[';
            for (int i = 0; i < al; i++) t->pool[t->pool_len++] = alt[i] == '\n' ? ' ' : alt[i];
            t->pool[t->pool_len++] = ']';
            n = t->pool_len - off;
        }
    }
    if (!grow((void **)&t->runs, &t->runs_cap, t->nruns + 1, sizeof(run_t))) return;
    run_t *r = &t->runs[t->nruns++];
    memset(r, 0, sizeof(*r));
    r->off = off;
    r->len = n;
    r->style = (uint8_t)(b->style | ST_DIM); r->link = (int16_t)b->link; r->img = (int16_t)idx;
    r->brk = (uint8_t)(t->nruns == 1 ? 0 : b->pending_brk);
    r->indent = (uint8_t)(b->indent + b->dd);
    b->pending_brk = 0; b->bullet = 0; b->run_open = 0; b->space = 0;
}

static int num_attr(const char *a, const char *e, const char *name)
{
    int n;
    const char *v = attr(a, e, name, &n);
    int x = 0;
    for (int i = 0; v && i < n && v[i] >= '0' && v[i] <= '9'; i++) x = x * 10 + (v[i] - '0');
    return x > 4000 ? 4000 : x;
}

/* an element CSS would have hidden: hidden, aria-hidden="true", style="display:none" */
static int is_hidden(const char *a, const char *e)
{
    int n;
    const char *v;
    for (const char *p = a; p + 6 <= e; p++)
        if ((p == a || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && !strncmp(p, "hidden", 6) &&
            (p + 6 == e || p[6] == ' ' || p[6] == '/' || p[6] == '>' || p[6] == '='))
            return 1;
    /* class names that by convention mean "not shown until asked for" (menus, screen-reader text) or
     * "not part of the article" (navigation boxes, edit links) */
    static const char *const hide[] = { "dropdown", "sr-only", "visually-hidden", "screen-reader", "skip-link",
        "noprint", "mw-editsection", "navbox", "vector-header", "vector-page-toolbar", "mw-jump-link",
        "vector-column-start", "vector-sticky", "cookie", "modal", "popup", "mw-portlet", 0 };
    v = attr(a, e, "class", &n);
    for (int i = 0; v && hide[i]; i++) {
        int hl = (int)strlen(hide[i]);
        for (int k = 0; k + hl <= n; k++)
            if (!strncmp(v + k, hide[i], (size_t)hl)) return 1;
    }
    v = attr(a, e, "aria-hidden", &n);
    if (v && n == 4 && !strncmp(v, "true", 4)) return 1;
    v = attr(a, e, "style", &n);
    for (int i = 0; v && i + 12 <= n; i++) {
        if (strncmp(v + i, "display", 7)) continue;
        int k = i + 7;
        while (k < n && (v[k] == ' ' || v[k] == ':')) k++;
        if (k + 4 <= n && !strncmp(v + k, "none", 4)) return 1;
    }
    return 0;
}

static void parse_html(tf_t *t, const char *s, uint32_t len)
{
    build_t b;
    memset(&b, 0, sizeof(b));
    b.t = t;
    b.link = -1;
    const char *end = s + len;
    int skip = 0;                     /* inside script/style/svg...: the name of the tag that ends it */
    char skip_tag[12] = "";
    int in_title = 0, tl = 0;
    int list_ol[8] = {0}, list_n[8] = {0};
    int bold_depth = 0;
    while (s < end) {
        if (*s == '<') {
            /* comments, doctype, CDATA */
            if (s + 3 < end && s[1] == '!' && s[2] == '-' && s[3] == '-') {
                const char *c = s + 4;
                while (c + 2 < end && !(c[0] == '-' && c[1] == '-' && c[2] == '>')) c++;
                s = c + 3 > end ? end : c + 3;
                continue;
            }
            const char *p = s + 1;
            int closing = 0;
            if (p < end && *p == '/') { closing = 1; p++; }
            if (p >= end || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '!' || *p == '?')) {
                if (!skip && !in_title) text(&b, '<');
                s++;
                continue;
            }
            char tag[12];
            int n = 0;
            while (p < end && *p != '>' && *p != ' ' && *p != '/' && *p != '\t' && *p != '\n' && *p != '\r') {
                if (n < 11) tag[n++] = (char)lower(*p);
                p++;
            }
            tag[n] = 0;
            const char *ab = p;
            char q = 0;
            while (p < end && (q || *p != '>')) {
                if (q) { if (*p == q) q = 0; }
                else if (*p == '"' || *p == '\'') q = *p;
                p++;
            }
            const char *ae = p;
            s = p < end ? p + 1 : end;

            if (skip) {
                /* the same element nested inside the skipped one does not end it */
                int void_tag = tag_is(tag, "img") || tag_is(tag, "br") || tag_is(tag, "input") || tag_is(tag, "meta") || tag_is(tag, "link") || tag_is(tag, "hr");
                if (!strcmp(tag, skip_tag) && !void_tag && !(ae > ab && ae[-1] == '/')) {
                    if (closing) { if (--skip == 0) skip_tag[0] = 0; }
                    else skip++;
                }
                continue;
            }
            if (!closing && (tag_is(tag, "nav") || is_hidden(ab, ae)) &&
                !tag_is(tag, "input") && !tag_is(tag, "img") && !tag_is(tag, "br") && !tag_is(tag, "body") && !tag_is(tag, "html")) {
                skip = 1;
                strcpy(skip_tag, tag);
                continue;
            }
            if (!closing && (tag_is(tag, "script") || tag_is(tag, "style") || tag_is(tag, "svg") || tag_is(tag, "template") ||
                             tag_is(tag, "iframe") || tag_is(tag, "object") || tag_is(tag, "select") || tag_is(tag, "math"))) {
                if (tag_is(tag, "script") || tag_is(tag, "style")) {
                    /* raw text: nothing in it is a tag, it ends at </script> or </style> */
                    int tl3 = (int)strlen(tag);
                    while (s + tl3 + 2 < end) {
                        if (s[0] == '<' && s[1] == '/') {
                            int k = 0;
                            while (k < tl3 && lower(s[2 + k]) == tag[k]) k++;
                            if (k == tl3) break;
                        }
                        s++;
                    }
                    continue;                  /* the closing tag itself is read (and ignored) next */
                }
                skip = 1;
                strcpy(skip_tag, tag);
                continue;
            }
            if (tag_is(tag, "title")) { in_title = !closing; continue; }
            if (tag_is(tag, "head") || tag[0] == '!' || tag[0] == '?' || tag_is(tag, "meta") || tag_is(tag, "link")) continue;

            if (tag_is(tag, "br")) { brk(&b, 1); b.run_open = 0; continue; }
            if (tag_is(tag, "hr")) { brk(&b, 2); put_bytes(&b, "\x01", 1); brk(&b, 2); continue; }   /* \x01 = a rule */
            if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
                brk(&b, 2);
                if (closing) b.style &= ~(ST_H1 | ST_H2 | ST_H3);
                else b.style |= tag[1] == '1' ? ST_H1 : tag[1] == '2' ? ST_H2 : ST_H3;
                restyle(&b);
                continue;
            }
            if (tag_is(tag, "ul") || tag_is(tag, "ol")) {
                brk(&b, b.indent ? 1 : 2);
                if (!closing && b.indent < 7) { b.indent++; list_ol[b.indent] = tag[0] == 'o'; list_n[b.indent] = 0; }
                else if (closing && b.indent) b.indent--;
                continue;
            }
            if (tag_is(tag, "li")) {
                if (closing) continue;
                brk(&b, 1);
                b.bullet = 1;
                if (b.indent && list_ol[b.indent]) {
                    char num[8];
                    int v = ++list_n[b.indent], k = 0;
                    char r[6];
                    int rl = 0;
                    do { r[rl++] = (char)('0' + v % 10); v /= 10; } while (v && rl < 5);
                    while (rl) num[k++] = r[--rl];
                    num[k++] = '.';
                    num[k++] = ' ';
                    put_bytes(&b, num, k);
                } else {
                    put_bytes(&b, "\xE2\x80\xA2 ", 4);
                }
                b.space = 0;
                continue;
            }
            if (tag_is(tag, "pre") || tag_is(tag, "textarea")) {
                brk(&b, 2);
                b.pre = !closing;
                b.style = closing ? (b.style & ~ST_PRE) : (b.style | ST_PRE);
                restyle(&b);
                continue;
            }
            if (tag_is(tag, "td") || tag_is(tag, "th")) {
                if (!closing) { if (b.run_open) put_bytes(&b, "   ", 3); b.space = 0; }
                continue;
            }
            if (tag_is(tag, "b") || tag_is(tag, "strong")) {
                bold_depth += closing ? -1 : 1;
                if (bold_depth < 0) bold_depth = 0;
                if (bold_depth) b.style |= ST_BOLD; else b.style &= ~ST_BOLD;
                restyle(&b);
                continue;
            }
            if (tag_is(tag, "small")) {
                if (closing) b.style &= ~ST_DIM; else b.style |= ST_DIM;
                restyle(&b);
                continue;
            }
            if (tag_is(tag, "a")) {
                if (closing) b.link = -1;
                else {
                    int hl;
                    const char *h = attr(ab, ae, "href", &hl);
                    b.link = h && hl ? add_link(t, h, hl) : -1;
                }
                restyle(&b);
                continue;
            }
            if (tag_is(tag, "img") && !closing) {
                int sl;
                const char *src = attr(ab, ae, "src", &sl);
                int aw = num_attr(ab, ae, "width"), ah = num_attr(ab, ae, "height");
                if (src && sl > 0 && strncmp(src, "data:", 5) && t->nimgs < IMG_MAX &&
                    !((aw && aw < 24) || (ah && ah < 24))) {                 /* not icons and spacers */
                    timg_t *im = &t->imgs[t->nimgs];
                    im->src = malloc((size_t)sl + 1);
                    if (im->src) {
                        int o = 0;
                        for (int i = 0; i < sl; i++) {
                            uint32_t cp;
                            int m = src[i] == '&' ? entity(src + i, src + sl, &cp) : 0;
                            if (m && cp > 0 && cp < 0x80) { im->src[o++] = (char)cp; i += m - 1; }
                            else im->src[o++] = src[i];
                        }
                        im->src[o] = 0;
                        im->aw = aw;
                        im->ah = ah;
                        int al0;
                        const char *alt0 = attr(ab, ae, "alt", &al0);
                        put_image(&b, t->nimgs++, alt0, alt0 ? al0 : 0);
                        continue;
                    }
                }
                int al;
                const char *alt = attr(ab, ae, "alt", &al);
                if (alt && al > 0 && !(aw && aw < 24)) {
                    int st = b.style;
                    b.style |= ST_DIM;
                    restyle(&b);
                    text(&b, '[');
                    attr_text(&b, alt, al, 80);
                    text(&b, ']');
                    b.style = st;
                    restyle(&b);
                }
                continue;
            }
            if (tag_is(tag, "input") && !closing) {
                int tl2, vl;
                const char *ty = attr(ab, ae, "type", &tl2);
                if (ty && tl2 == 6 && !strncmp(ty, "hidden", 6)) continue;
                const char *v = attr(ab, ae, "value", &vl);
                if (!v || !vl) v = attr(ab, ae, "placeholder", &vl);
                if (!v || !vl) continue;           /* an empty field: nothing to show */
                text(&b, ' ');
                text(&b, '[');
                attr_text(&b, v, vl, 40);
                text(&b, ']');
                text(&b, ' ');
                continue;
            }
            if (tag_is(tag, "button") || tag_is(tag, "option")) { text(&b, ' '); continue; }
            if (tag_is(tag, "dd") || tag_is(tag, "dt") || tag_is(tag, "dl")) {
                brk(&b, tag[1] == 'l' ? 2 : 1);
                b.dd = !closing && tag[1] == 'd';
                continue;
            }
            if (tag_is(tag, "p") || tag_is(tag, "blockquote")) { brk(&b, 2); continue; }
            if (is_block(tag)) { brk(&b, 1); continue; }
            continue;                       /* span, em, i, code, font, ...: no change we can show */
        }
        if (skip) { s++; continue; }
        uint32_t cp;
        if (*s == '&') {
            int n = entity(s, end, &cp);
            if (n) s += n; else { cp = '&'; s++; }
        } else {
            cp = utf8(&s, end);
        }
        if (in_title) {
            if (cp == '\n' || cp == '\t' || cp == '\r') cp = ' ';
            if (cp < 0x80 && tl < (int)sizeof(t->title) - 1 && !(cp == ' ' && (!tl || t->title[tl - 1] == ' '))) t->title[tl++] = (char)cp;
            else if (cp >= 0x80 && cp < 0x800 && tl < (int)sizeof(t->title) - 2) {
                t->title[tl++] = (char)(0xC0 | (cp >> 6));
                t->title[tl++] = (char)(0x80 | (cp & 0x3F));
            }
            t->title[tl] = 0;
            continue;
        }
        if (cp) text(&b, cp);
    }
    while (tl && t->title[tl - 1] == ' ') t->title[--tl] = 0;
}

static void parse_text(tf_t *t, const char *s, uint32_t len)
{
    build_t b;
    memset(&b, 0, sizeof(b));
    b.t = t;
    b.link = -1;
    b.pre = 1;
    b.style = ST_PRE;
    const char *end = s + len;
    while (s < end) text(&b, utf8(&s, end));
}

/* ------------------------------------------------------------------ layout */

static const ui_font_t *font_of(int style)
{
    if (style & (ST_H1 | ST_H2)) return &ui_font_title;
    if (style & (ST_H3 | ST_BOLD)) return &ui_font_medium;
    if (style & ST_DIM) return &ui_font_small;
    return &ui_font_regular;
}

static int text_w(const ui_font_t *f, const char *s, int n)
{
    char tmp[256];
    if (n > 255) n = 255;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    return gfx_text_width(f, tmp);
}

static void add_box(tf_t *t, int x, int y, int w, uint32_t off, int len, int style, int link)
{
    if (!grow((void **)&t->boxes, &t->boxes_cap, t->nboxes + 1, sizeof(box_t))) return;
    box_t *bx = &t->boxes[t->nboxes++];
    bx->x = (int16_t)x; bx->y = y; bx->w = (int16_t)w;
    bx->off = off; bx->len = (uint16_t)len;
    bx->style = (uint8_t)style; bx->link = (int16_t)link;
    bx->img = -1; bx->h = 0;
}

/* how big picture i is shown, laid out in a column avail wide (0 x 0 = nothing to show yet) */
static void img_size(tf_t *t, int i, int avail, int *w, int *h)
{
    timg_t *im = &t->imgs[i];
    *w = *h = 0;
    if (im->state == IMG_OK) { *w = im->im.w; *h = im->im.h; }
    else if (im->state != IMG_FAILED && im->aw && im->ah) { *w = im->aw; *h = im->ah; }
    if (*w > avail && avail > 0) { *h = *h * avail / *w; *w = avail; }
}

static void layout(tf_t *t, int width)
{
    t->nboxes = 0;
    t->laid_w = width;
    int right = width - MARGIN;
    int y = 16, x = MARGIN, line_h = 0, left = MARGIN;
    int line_start = 0;
    for (int r = 0; r < t->nruns; r++) {
        run_t *ru = &t->runs[r];
        const ui_font_t *f = font_of(ru->style);
        int lh = f->line_h + ((ru->style & (ST_H1 | ST_H2)) ? 6 : 0);
        if (ru->brk || r == 0) {
            if (r) {
                y += line_h ? line_h : ui_font_regular.line_h;
                if (ru->brk == 2) y += 10;
            }
            left = MARGIN + ru->indent * 22;
            x = left;
            line_h = 0;
            line_start = t->nboxes;
        }
        (void)line_start;
        if (ru->bullet) left = x + text_w(f, t->pool + ru->off, ru->len > 4 ? 4 : (int)ru->len);
        if (ru->img >= 0) {
            int iw, ih;
            img_size(t, ru->img, right - left, &iw, &ih);
            if (!iw || !ih) {
                if (t->imgs[ru->img].state != IMG_FAILED || !ru->len) continue;
                goto as_text;                           /* its alt text instead */
            }
            if (x + iw > right && x > left) { y += line_h ? line_h : lh; x = left; line_h = 0; }
            add_box(t, x, y, iw, ru->off, 0, ru->style, ru->link);
            t->boxes[t->nboxes - 1].img = ru->img;
            t->boxes[t->nboxes - 1].h = (int16_t)ih;
            if (ih + 6 > line_h) line_h = ih + 6;
            x += iw + 6;
            continue;
        }
    as_text:;
        const char *s = t->pool + ru->off;
        int n = (int)ru->len;
        if (n == 1 && s[0] == '\x01') {                 /* <hr> */
            add_box(t, MARGIN, y + 8, width - 2 * MARGIN, ru->off, 0, 0xFF, -1);
            line_h = 16;
            continue;
        }
        int i = 0;
        while (i < n) {
            /* the next word, with the spaces after it */
            int j = i;
            while (j < n && s[j] != ' ') j++;
            int k = j;
            while (k < n && s[k] == ' ') k++;
            int ww = text_w(f, s + i, j - i);
            int sw = (k - j) * (f->size / 3 + 1);
            if (x + ww > right && x > left) {           /* onto the next line */
                y += line_h ? line_h : lh;
                x = left;
                line_h = 0;
            }
            if (ww > right - left && !(ru->style & ST_PRE)) {
                /* a word wider than the page (a long address): broken wherever the line is full */
                int p0 = i;
                while (p0 < j) {
                    int p1 = p0 + 1;
                    while (p1 < j && ((unsigned char)s[p1] & 0xC0) == 0x80) p1++;
                    for (;;) {
                        int nx = p1;
                        if (nx >= j) break;
                        nx++;
                        while (nx < j && ((unsigned char)s[nx] & 0xC0) == 0x80) nx++;
                        if (x + text_w(f, s + p0, nx - p0) > right) break;
                        p1 = nx;
                    }
                    int pw = text_w(f, s + p0, p1 - p0);
                    add_box(t, x, y, pw, ru->off + (uint32_t)p0, p1 - p0, ru->style, ru->link);
                    if (lh > line_h) line_h = lh;
                    x += pw;
                    p0 = p1;
                    if (p0 < j) { y += line_h; x = left; line_h = 0; }
                }
            } else if (j > i) {
                add_box(t, x, y, ww, ru->off + (uint32_t)i, j - i, ru->style, ru->link);
                if (lh > line_h) line_h = lh;
                x += ww;
            }
            x += sw;
            i = k;
        }
    }
    t->doc_h = y + (line_h ? line_h : 0) + 32;
}

/* ------------------------------------------------------------------ addresses */

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* href on the page at base -> an absolute address in out; 0 = can't be followed */
static int resolve(const char *base, const char *href, char *out)
{
    while (*href == ' ') href++;
    if (starts(href, "http://") || starts(href, "https://")) { strncpy(out, href, URL_MAX - 1); out[URL_MAX - 1] = 0; return 1; }
    if (starts(href, "javascript:") || starts(href, "mailto:") || starts(href, "tel:") || starts(href, "data:")) return 0;
    if (href[0] == '#') return 0;
    /* base = scheme://host[:port]/path */
    const char *hs = strstr(base, "://");
    if (!hs) return 0;
    hs += 3;
    const char *pe = hs;
    while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    int origin = (int)(pe - base);
    char tmp[URL_MAX * 2];
    if (href[0] == '/' && href[1] == '/') {
        int sl = (int)(hs - 2 - base);
        memcpy(tmp, base, (size_t)sl);
        tmp[sl] = 0;
        strcat(tmp, href);
    } else if (href[0] == '/') {
        memcpy(tmp, base, (size_t)origin);
        tmp[origin] = 0;
        int l = (int)strlen(tmp);
        strncpy(tmp + l, href, sizeof(tmp) - (size_t)l - 1);
        tmp[sizeof(tmp) - 1] = 0;
    } else if (href[0] == '?') {
        const char *q = pe;
        while (*q && *q != '?' && *q != '#') q++;
        int l = (int)(q - base);
        memcpy(tmp, base, (size_t)l);
        tmp[l] = 0;
        int m = (int)strlen(tmp);
        strncpy(tmp + m, href, sizeof(tmp) - (size_t)m - 1);
        tmp[sizeof(tmp) - 1] = 0;
    } else {
        /* relative to the directory of the base path */
        const char *q = pe;
        while (*q && *q != '?' && *q != '#') q++;
        const char *slash = 0;
        for (const char *c = pe; c < q; c++) if (*c == '/') slash = c;
        int l = slash ? (int)(slash + 1 - base) : origin;
        memcpy(tmp, base, (size_t)l);
        tmp[l] = 0;
        if (!slash) strcat(tmp, "/");
        int m = (int)strlen(tmp);
        strncpy(tmp + m, href, sizeof(tmp) - (size_t)m - 1);
        tmp[sizeof(tmp) - 1] = 0;
    }
    /* fold "./" and "../" in the path */
    char *path = strstr(tmp, "://");
    path = path ? path + 3 : tmp;
    while (*path && *path != '/') path++;
    char outp[URL_MAX * 2];
    int o = 0;
    int pl = (int)(path - tmp);
    memcpy(outp, tmp, (size_t)pl);
    o = pl;
    const char *c = path;
    while (*c && *c != '?' && *c != '#') {
        if (starts(c, "/./")) { c += 2; continue; }
        if (starts(c, "/../") || !strcmp(c, "/..")) {
            while (o > pl && outp[o - 1] != '/') o--;
            if (o > pl) o--;
            c += 3;
            if (!*c) { outp[o++] = '/'; }
            continue;
        }
        outp[o++] = *c++;
        if (o >= URL_MAX - 1) break;
    }
    while (*c && *c != '#' && o < URL_MAX - 1) outp[o++] = *c++;
    outp[o] = 0;
    strncpy(out, outp, URL_MAX - 1);
    out[URL_MAX - 1] = 0;
    return 1;
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

/* ------------------------------------------------------------------ loading */

static const char home_page[] =
    "<title>tFire</title>"
    "<h1>tFire</h1>"
    "<p><b>Project tFire, update 2.</b> tFire, the web browser for tOS, now shows pictures (PNG and JPEG) "
    "and hides what a page keeps out of sight &mdash; menus, navigation, hidden text &mdash; so you start at the content. "
    "It opens web pages over HTTPS and HTTP, follows links and keeps your history. "
    "Style sheets and scripts come in the next updates, with parts of Firefox's own source.</p>"
    "<h3>Try</h3>"
    "<ul>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Firefox\">Firefox on Wikipedia</a> &mdash; with its pictures</li>"
    "<li><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo</a> &mdash; search the web</li>"
    "<li><a href=\"https://text.npr.org/\">NPR News</a> &mdash; the news, as plain text</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> &mdash; today's headlines, in plain pages</li>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first website ever (CERN, 1991)</a></li>"
    "<li><a href=\"https://update.artfical.com/\">tOS updates</a></li>"
    "</ul>"
    "<p>Type an address in the bar above, or words to search for them.</p>"
    "<h3>What works</h3>"
    "<ul><li>Headings, paragraphs, lists, links, bold text, preformatted text</li>"
    "<li>Pictures: PNG and JPEG (WebP and progressive JPEG show their description)</li>"
    "<li>Redirects, back and forward, reload</li>"
    "<li>Plain text files</li></ul>";

static void set_title(uiapp_t *a, tf_t *t)
{
    strcpy(a->title, "tFire");
    if (t->title[0] && strcmp(t->title, "tFire")) {
        strcat(a->title, " \xE2\x80\x94 ");
        int l = (int)strlen(a->title);
        strncpy(a->title + l, t->title, sizeof(a->title) - (size_t)l - 1);
        a->title[sizeof(a->title) - 1] = 0;
    }
}

static void show_message(uiapp_t *a, tf_t *t, const char *head, const char *msg)
{
    page_free(t);
    static char html[1024];
    strcpy(html, "<title>");
    strcat(html, head);
    strcat(html, "</title><h2>");
    strcat(html, head);
    strcat(html, "</h2><p>");
    int l = (int)strlen(html);
    for (const char *c = msg; *c && l < 900; c++) {
        if (*c == '<') { strcpy(html + l, "&lt;"); l += 4; }
        else if (*c == '&') { strcpy(html + l, "&amp;"); l += 5; }
        else html[l++] = *c;
    }
    html[l] = 0;
    strcat(html, "</p><p><small>");
    l = (int)strlen(html);
    for (const char *c = t->url; *c && l < 1000; c++) { if (*c != '<' && *c != '&') html[l++] = *c; }
    html[l] = 0;
    strcat(html, "</small></p>");
    parse_html(t, html, (uint32_t)strlen(html));
    set_title(a, t);
    a->dirty = 1;
}

/* go to url; record = put it in the history */
static void go(uiapp_t *a, tf_t *t, const char *url, int record)
{
    if (t->fetch) { uihost_fetch_free(t->fetch); t->fetch = 0; }
    strncpy(t->url, url, URL_MAX - 1);
    t->url[URL_MAX - 1] = 0;
    strcpy(t->field, t->url);
    t->editing = 0;
    if (record) {
        if (t->hpos < t->nhist - 1) t->nhist = t->hpos + 1;
        if (t->nhist == HIST_MAX) { for (int i = 1; i < HIST_MAX; i++) strcpy(t->hist[i - 1], t->hist[i]); t->nhist--; }
        strcpy(t->hist[t->nhist], t->url);
        t->hpos = t->nhist++;
    }
    if (!strcmp(url, HOME)) {
        page_free(t);
        parse_html(t, home_page, sizeof(home_page) - 1);
        set_title(a, t);
        a->dirty = 1;
        return;
    }
    t->fetch = uihost_fetch(url);
    t->load_start = uihost_ms();
    if (!t->fetch) show_message(a, t, "Can't open the page", "tOS could not start fetching it (out of memory?).");
    a->dirty = 1;
}

static void loaded(uiapp_t *a, tf_t *t)
{
    uint32_t len;
    int status;
    const char *ctype, *final, *err;
    int st = uihost_fetch_poll(t->fetch);
    const uint8_t *body = uihost_fetch_result(t->fetch, &len, &status, &ctype, &final, &err);
    if (st < 0) {
        show_message(a, t, "Can't open the page", err && err[0] ? err : "The page could not be fetched.");
    } else {
        if (final && final[0] && strcmp(final, t->url)) {
            strncpy(t->url, final, URL_MAX - 1);
            if (t->hpos >= 0 && t->hpos < t->nhist) strcpy(t->hist[t->hpos], t->url);
            if (!t->editing) strcpy(t->field, t->url);
        }
        page_free(t);
        if (starts(ctype, "text/html") || starts(ctype, "application/xhtml") || !ctype[0])
            parse_html(t, (const char *)body, len);
        else if (starts(ctype, "text/"))
            parse_text(t, (const char *)body, len);
        else {
            char msg[160];
            strcpy(msg, "This is a file of type ");
            strcat(msg, ctype);
            strcat(msg, ", which tFire cannot show yet.");
            uihost_fetch_free(t->fetch);
            t->fetch = 0;
            show_message(a, t, "Can't show this file", msg);
            return;
        }
        if (!t->nruns && status >= 400) {
            uihost_fetch_free(t->fetch);
            t->fetch = 0;
            show_message(a, t, "The server said no", status == 404 ? "There is no page at this address (404)." : "The server answered with an error.");
            return;
        }
        set_title(a, t);
    }
    uihost_fetch_free(t->fetch);
    t->fetch = 0;
    a->dirty = 1;
}

/* ------------------------------------------------------------------ the window */

static void tf_init(uiapp_t *a, const char *arg)
{
    tf_t *t = a->st;
    t->hpos = -1;
    go(a, t, arg && arg[0] ? arg : HOME, 1);
}

static void tf_close(uiapp_t *a)
{
    tf_t *t = a->st;
    if (t->fetch) uihost_fetch_free(t->fetch);
    t->fetch = 0;
    page_free(t);
}

#define BTN 32
static int back_x(void) { return 10; }
static int fwd_x(void) { return 10 + BTN + 4; }
static int reload_x(void) { return 10 + 2 * (BTN + 4); }
static int home_x(void) { return 10 + 3 * (BTN + 4); }
static int field_x(void) { return 10 + 4 * (BTN + 4) + 6; }

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

static void tf_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    tf_t *t = a->st;
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
        gfx_clip(&c, fx + 10, y + 8, fw - 20, 30);
        const char *shown = t->field;
        int tw = gfx_text_width(&ui_font_regular, shown);
        int tx = fx + 12;
        if (t->editing && tw > fw - 30) tx = fx + fw - 18 - tw;         /* keep the end (and the caret) in view */
        if (t->editing && t->select_all && t->field[0]) gfx_fill_alpha(&c, tx - 2, y + 13, tw + 4, 20, UI_ACCENT, 70);
        gfx_text(&c, &ui_font_regular, tx, y + 14, shown, t->editing ? UI_TEXT : UI_DIM, 255);
        if (t->editing && !t->select_all && (uihost_ms() / 500) % 2 == 0) gfx_fill(&c, tx + tw + 1, y + 14, 2, 18, UI_TEXT);
    }
    /* loading: a bar sweeping along the bottom of the toolbar */
    if (t->fetch) {
        int p = (int)((uihost_ms() - t->load_start) / 4 % (uint32_t)(w + 160)) - 160;
        gfx_surface_t c = *s;
        gfx_clip(&c, x, y + TB_H - 3, w, 3);
        gfx_fill(&c, x + p, y + TB_H - 3, 160, 3, UI_ACCENT);
    }

    /* the page: black on white, the way pages expect */
    int py = y + TB_H, ph = h - TB_H;
    gfx_fill(s, x, py, w, ph, 0xFFFFFF);
    if (t->laid_w != w) layout(t, w);
    int maxs = t->doc_h - ph;
    if (maxs < 0) maxs = 0;
    if (t->scroll > maxs) t->scroll = maxs;
    if (t->scroll < 0) t->scroll = 0;
    gfx_surface_t c = *s;
    gfx_clip(&c, x, py, w, ph);
    char tmp[260];
    for (int i = 0; i < t->nboxes; i++) {
        box_t *b = &t->boxes[i];
        int by = py + b->y - t->scroll;
        if (by > py + ph) break;
        if (by + (b->img >= 0 ? b->h : 40) < py) continue;
        if (b->style == 0xFF) { gfx_hline(&c, x + b->x, by, b->w, 0xD4D4D8, 255); continue; }
        if (b->img >= 0) {
            timg_t *im = &t->imgs[b->img];
            if (im->state == IMG_OK) {
                if (im->im.w == b->w) gfx_blit(&c, &im->im, x + b->x, by, 255);
                else gfx_blit_scaled(&c, &im->im, x + b->x, by, b->w, b->h, 255);
            } else {
                gfx_fill(&c, x + b->x, by, b->w, b->h, 0xF1F1F4);           /* still coming */
            }
            if (b->link >= 0) gfx_rrect_outline(&c, x + b->x - 1, by - 1, b->w + 2, b->h + 2, 2, 0x1A56DB, 90);
            continue;
        }
        const ui_font_t *f = font_of(b->style);
        int n = b->len > 255 ? 255 : b->len;
        memcpy(tmp, t->pool + b->off, (size_t)n);
        tmp[n] = 0;
        uint32_t col = b->link >= 0 ? 0x1A56DB : (b->style & ST_DIM) ? 0x6B7280 : 0x111827;
        gfx_text(&c, f, x + b->x, by, tmp, col, 255);
        if (b->link >= 0) {
            int uw = b->w;
            if (i + 1 < t->nboxes && b[1].link == b->link && b[1].y == b->y) uw = b[1].x - b->x;
            gfx_hline(&c, x + b->x, by + f->ascent + 2, uw, col, 160);
        }
    }
    /* scroll bar */
    if (maxs > 0) {
        int bh = ph * ph / t->doc_h;
        if (bh < 30) bh = 30;
        int bt = py + (ph - bh) * t->scroll / maxs;
        gfx_rrect(s, x + w - 8, bt + 2, 5, bh - 4, 2, 0x000000, 70);
    }
}

static void tf_press(uiapp_t *a, int px, int py, int button)
{
    (void)button;
    tf_t *t = a->st;
    a->dirty = 1;
    if (py < TB_H) {
        if (ui_hit(px, py, back_x(), 6, BTN, 34)) { if (t->hpos > 0) { t->hpos--; go(a, t, t->hist[t->hpos], 0); } return; }
        if (ui_hit(px, py, fwd_x(), 6, BTN, 34)) { if (t->hpos < t->nhist - 1) { t->hpos++; go(a, t, t->hist[t->hpos], 0); } return; }
        if (ui_hit(px, py, reload_x(), 6, BTN, 34)) { go(a, t, t->url, 0); return; }
        if (ui_hit(px, py, home_x(), 6, BTN, 34)) { go(a, t, HOME, 1); return; }
        if (px >= field_x()) {
            if (!t->editing) { t->editing = 1; t->select_all = 1; strcpy(t->field, t->url); }
            else t->select_all = 0;
        }
        return;
    }
    if (t->editing) { t->editing = 0; strcpy(t->field, t->url); }
    int dy = py - TB_H + t->scroll;
    for (int i = 0; i < t->nboxes; i++) {
        box_t *b = &t->boxes[i];
        if (b->link < 0 || b->style == 0xFF) continue;
        int lh = b->img >= 0 ? b->h : font_of(b->style)->line_h;
        if (px >= b->x - 2 && px < b->x + b->w + 4 && dy >= b->y - 2 && dy < b->y + lh) {
            char next[URL_MAX];
            if (resolve(t->url, t->links[b->link], next)) go(a, t, next, 1);
            return;
        }
    }
}

static void tf_key(uiapp_t *a, int k)
{
    tf_t *t = a->st;
    a->dirty = 1;
    if (t->editing) {
        int l = (int)strlen(t->field);
        if (k == '\n') {
            char url[URL_MAX];
            typed_to_url(t->field, url);
            go(a, t, url, 1);
        } else if (k == UI_KEY_ESC) {
            t->editing = 0;
            strcpy(t->field, t->url);
        } else if (k == '\b') {
            if (t->select_all) { t->field[0] = 0; t->select_all = 0; }
            else if (l) {
                while (l > 1 && ((unsigned char)t->field[l - 1] & 0xC0) == 0x80) l--;     /* a whole UTF-8 character */
                t->field[l - 1] = 0;
            }
        } else if (k >= 32 && k < 256 && k != 127) {
            if (t->select_all) { t->field[0] = 0; l = 0; t->select_all = 0; }
            if (k < 0x80 && l < URL_MAX - 1) { t->field[l] = (char)k; t->field[l + 1] = 0; }
            else if (k >= 0x80 && l < URL_MAX - 2) { t->field[l] = (char)(0xC0 | (k >> 6)); t->field[l + 1] = (char)(0x80 | (k & 0x3F)); t->field[l + 2] = 0; }
        }
        return;
    }
    int page = a->h - TB_H - 40;
    if (k == UI_KEY_DOWN) t->scroll += 40;
    else if (k == UI_KEY_UP) t->scroll -= 40;
    else if (k == ' ') t->scroll += page;
    else if (k == '\b' || k == UI_KEY_LEFT) { if (t->hpos > 0) { t->hpos--; go(a, t, t->hist[t->hpos], 0); } }
    else if (k == UI_KEY_RIGHT) { if (t->hpos < t->nhist - 1) { t->hpos++; go(a, t, t->hist[t->hpos], 0); } }
    else if (k == 'l' || k == '/') { t->editing = 1; t->select_all = 1; strcpy(t->field, t->url); }
}

static void tf_wheel(uiapp_t *a, int delta)
{
    tf_t *t = a->st;
    t->scroll += delta * 64;
    if (t->scroll < 0) t->scroll = 0;
    a->dirty = 1;
}

/* the pictures, one at a time after the page: 1 = one arrived (the page needs laying out again) */
static int imgs_step(tf_t *t)
{
    if (t->img_fetch) {
        int st = uihost_fetch_poll(t->img_fetch);
        if (st == 0) return 0;
        timg_t *im = &t->imgs[t->img_cur];
        im->state = IMG_FAILED;
        if (st > 0) {
            uint32_t len;
            int status;
            const char *ctype;
            const uint8_t *d = uihost_fetch_result(t->img_fetch, &len, &status, &ctype, 0, 0);
            uint8_t *px;
            int w, h;
            int mw = im->aw ? im->aw : IMG_MAX_W, mh = im->ah ? im->ah : 0;
            if (mw > IMG_MAX_W) { if (mh) mh = mh * IMG_MAX_W / mw; mw = IMG_MAX_W; }
            if (status < 400 && uihost_image_decode(d, len, mw, mh, &px, &w, &h) == 0) {
                im->im.rgba = px; im->im.w = w; im->im.h = h;
                im->state = IMG_OK;
            }
        }
        uihost_fetch_free(t->img_fetch);
        t->img_fetch = 0;
        return 1;
    }
    for (int i = 0; i < t->nimgs; i++) {
        if (t->imgs[i].state != IMG_WAIT) continue;
        char url[URL_MAX];
        if (!resolve(t->url, t->imgs[i].src, url)) { t->imgs[i].state = IMG_FAILED; continue; }
        t->img_fetch = uihost_fetch(url);
        if (!t->img_fetch) return 0;
        t->imgs[i].state = IMG_LOADING;
        t->img_cur = i;
        return 0;
    }
    return 0;
}

static int tf_tick(uiapp_t *a)
{
    tf_t *t = a->st;
    if (t->fetch) {
        if (uihost_fetch_poll(t->fetch) != 0) loaded(a, t);
        return 1;                               /* the loading bar moves */
    }
    if (t->nimgs && imgs_step(t)) { t->laid_w = 0; return 1; }
    if (t->editing) {
        uint32_t ph = uihost_ms() / 500;
        if (ph != t->blink) { t->blink = ph; return 1; }
    }
    return 0;
}

const uiapp_class_t app_tfire = {
    "tfire", "tFire", "tfire", 900, 620, 1, sizeof(tf_t),
    tf_init, tf_draw, tf_press, 0, 0, tf_key, tf_wheel, tf_tick, tf_close,
    0, 0, 0,
};
