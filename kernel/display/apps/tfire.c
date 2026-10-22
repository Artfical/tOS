#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"
#include "engine.h"

/*
 * tFire, the web browser (Project tFire, update 3: the engine).
 *
 * The window around the engine (kernel/tfire): the toolbar, the address bar and the history are here; the page
 * itself (HTML parsed by NetSurf's libhubbub, CSS by libcss, scripts by QuickJS, tFire's own layout) comes from
 * engine.c as a list of things to draw, which this draws with the desktop's fonts.
 */

#define TB_H     46              /* toolbar */
#define DL_H     40              /* the download bar, along the top of the page */
#define DL_SHOW_X  184           /* its buttons, from the right edge */
#define DL_CLOSE_X 44
#define URL_MAX  1024
#define HIST_MAX 32
#define SEARCH   "https://html.duckduckgo.com/html/?q="
#define HOME     "tfire:home"

void tf_free(void *p);           /* (what the engine allocates, it frees: tfire/libc/tf_alloc.c) */
extern const char tfire_notices[];   /* THIRD_PARTY_NOTICES.html, the licences (tfire_notices.s) */

typedef struct {
    char url[URL_MAX];            /* the page shown (or being loaded) */
    char field[URL_MAX];          /* the address bar */
    int editing, select_all;
    char hist[HIST_MAX][URL_MAX];
    int nhist, hpos;
    tf_page *page;
    uint32_t load_start;
    int scroll;
    int view_w, view_h;           /* the page's part of the window, when last drawn */
    uint32_t blink;
    uiapp_t *app;
    char status[URL_MAX];         /* the link under the pointer, shown at the bottom */
    /* the page that was shown, kept on screen until the next one's answer is here (and back if that is a download) */
    tf_page *prev;
    char prev_url[URL_MAX];
    int prev_hpos, prev_nhist;
    /* the last download, in a bar at the bottom */
    char dl_text[256];
    char dl_dir[200];
    int dl_ok;
} tf_t;

/* the page on screen: the one that was there, while the new one is still coming */
static tf_page *shown(tf_t *t) { return t->prev ? t->prev : t->page; }

/* ------------------------------------------------------------------ fonts for the engine */

static const ui_font_t *const fonts[] = { &ui_font_small, &ui_font_regular, &ui_font_medium, &ui_font_title, &ui_font_huge };

static int font_of(int px, int bold)
{
    if (px <= 12) return 0;
    if (px <= 17) return bold ? 2 : 1;
    if (px <= 36) return 3;
    return 4;
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

static const char home_page[] =
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
    "<div class=top><h1>tFire</h1><p>Project tFire, update 3: the engine. CSS, JavaScript and the web as it is.</p></div>"
    "<div class=wrap>"
    "<p id=clock>&nbsp;</p>"
    "<div class=card><a href=\"https://en.wikipedia.org/wiki/Firefox\">Wikipedia</a> <span>&mdash; Firefox, with its style sheets and pictures</span></div>"
    "<div class=card><a href=\"https://news.ycombinator.com/\">Hacker News</a> <span>&mdash; tables, forms, scripts</span></div>"
    "<div class=card><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo</a> <span>&mdash; search the web</span></div>"
    "<div class=card><a href=\"https://www.bing.com/\">Bing</a> <span>&mdash; search, with its scripts</span></div>"
    "<div class=card><a href=\"https://text.npr.org/\">NPR News</a> <span>&mdash; the news</span></div>"
    "<div class=card><a href=\"https://update.artfical.com/\">tOS updates</a></div>"
    "<h3>The engine</h3><ul>"
    "<li>HTML5 parsing (NetSurf's libhubbub), CSS 2.1 and much of CSS 3 (libcss): selectors, the cascade, media queries</li>"
    "<li>Layout: blocks, inline text, tables, flex rows, lists, form fields, pictures (PNG, JPEG)</li>"
    "<li>JavaScript: QuickJS (ES2023) with the DOM, events, timers, fetch, XMLHttpRequest, cookies and localStorage</li>"
    "<li>Forms: typing, check boxes, lists, GET and POST</li></ul>"
    "<p id=js style=\"color:#9ca3af\">JavaScript is off.</p>"
    "<p><a href=\"tfire:licenses\">Licences</a> &mdash; the free software tFire is built from</p>"
    "</div>"
    "<script>"
    "const js = document.getElementById('js');"
    "js.textContent = 'JavaScript is on: ' + navigator.userAgent;"
    "js.style.color = '#059669';"
    "function tick() { document.getElementById('clock').textContent = new Date().toUTCString(); }"
    "tick(); setInterval(tick, 1000);"
    "</script></body></html>";

/* ------------------------------------------------------------------ loading */

static void set_title(uiapp_t *a, tf_t *t)
{
    strcpy(a->title, "tFire");
    const char *pt = t->page ? page_title(t->page) : "";
    if (pt[0] && strcmp(pt, "tFire")) {
        strcat(a->title, " \xE2\x80\x94 ");
        int l = (int)strlen(a->title);
        strncpy(a->title + l, pt, sizeof(a->title) - (size_t)l - 1);
        a->title[sizeof(a->title) - 1] = 0;
    }
}

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
    if (t->page) {
        if (t->prev) page_close(t->prev);                    /* (one still waiting for its answer: gone) */
        else { strcpy(t->prev_url, t->url); t->prev_hpos = t->hpos; t->prev_nhist = t->nhist; }
        t->prev = t->page;
        t->page = 0;
    }
    strncpy(t->url, req->url, URL_MAX - 1);
    t->url[URL_MAX - 1] = 0;
    strcpy(t->field, t->url);
    t->editing = 0;
    if (!t->prev) t->scroll = 0;
    if (record && !req->replace) {
        if (t->hpos < t->nhist - 1) t->nhist = t->hpos + 1;
        if (t->nhist == HIST_MAX) { for (int i = 1; i < HIST_MAX; i++) strcpy(t->hist[i - 1], t->hist[i]); t->nhist--; }
        strcpy(t->hist[t->nhist], t->url);
        t->hpos = t->nhist++;
    } else if (req->replace && t->hpos >= 0 && t->hpos < t->nhist) strcpy(t->hist[t->hpos], t->url);
    if (!strcmp(req->url, HOME)) t->page = page_open_html(HOME, home_page, sizeof(home_page) - 1, &host);
    else if (!strcmp(req->url, "tfire:licenses")) t->page = page_open_html("tfire:licenses", tfire_notices, (int)strlen(tfire_notices), &host);
    else t->page = page_open(req, &host);
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

/* ------------------------------------------------------------------ the window */

static void tf_init(uiapp_t *a, const char *arg)
{
    tf_t *t = a->st;
    t->app = a;
    t->hpos = -1;
    t->view_w = 900;
    t->view_h = 560;
    go(a, t, arg && arg[0] ? arg : HOME, 1);
}

static void tf_close(uiapp_t *a)
{
    tf_t *t = a->st;
    if (t->page) page_close(t->page);
    if (t->prev) page_close(t->prev);
    t->page = t->prev = 0;
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
    const ui_font_t *f = fonts[it->font < 5 ? it->font : 1];
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
    if (t->page && page_loading(t->page)) {
        int p = (int)((uihost_ms() - t->load_start) / 4 % (uint32_t)(w + 160)) - 160;
        gfx_surface_t c = *s;
        gfx_clip(&c, x, y + TB_H - 3, w, 3);
        gfx_fill(&c, x + p, y + TB_H - 3, 160, 3, UI_ACCENT);
    }

    /* the page */
    int py = y + TB_H, ph = h - TB_H;
    t->view_w = w;
    t->view_h = ph;
    tf_page *pg = shown(t);
    tf_layout *L = pg ? page_layout(pg) : 0;
    gfx_fill(s, x, py, w, ph, L && L->n ? L->page_bg : 0xFFFFFF);
    if (!L) return;
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
        int reach = it->rot ? (it->w > it->h ? it->w : it->h) : 0;     /* (turned: its corners reach further) */
        if (iy - reach > py + ph || iy + it->h + reach < py) continue;
        switch (it->kind) {
        case IT_RECT: case IT_TEXT: case IT_IMAGE:
            tfp_item(&c, pg, L, it, ix, iy, fonts);
            break;
        case IT_FIELD:
            draw_field(&c, t, L, it, ix, iy);
            break;
        default:
            break;
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
    /* scroll bar */
    if (maxs > 0) {
        int bh = ph * ph / doc_h;
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
    if (t->dl_text[0] && py < TB_H + DL_H) {                       /* the download bar */
        if (t->dl_ok && px >= t->view_w - DL_SHOW_X && px < t->view_w - DL_SHOW_X + 130) uihost_open("files", t->dl_dir);
        else if (px >= t->view_w - DL_CLOSE_X) t->dl_text[0] = 0;
        return;
    }
    if (shown(t)) page_click(shown(t), px, py - TB_H + t->scroll);
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
    /* the page first (a form field, or a script that wants keys), then scrolling */
    if (t->page && page_key(t->page, k)) return;
    int page = t->view_h - 40;
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

static int tf_tick(uiapp_t *a)
{
    tf_t *t = a->st;
    int redraw = 0;
    if (!t->page) return 0;
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
                strcpy(t->field, t->url);
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
        if (!t->editing) strcpy(t->field, u);
        if (t->hpos >= 0 && t->hpos < t->nhist) strcpy(t->hist[t->hpos], u);
        redraw = 1;
    }
    int sy = page_take_scroll(t->page);
    if (sy >= 0) { t->scroll = sy; redraw = 1; }
    page_scrolled(t->page, t->scroll);
    if (redraw) set_title(a, t);
    if (page_loading(t->page)) redraw = 1;                   /* the loading bar moves */
    if (page_focus(t->page) || t->editing) {
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
    int in = px >= 0 && py >= TB_H && !(t->dl_text[0] && py < TB_H + DL_H);
    if (page_hover(pg, in ? px : -1, in ? py - TB_H + t->scroll : -1)) a->dirty = 1;
    const char *href = in ? page_link_at(pg, px, py - TB_H + t->scroll) : 0;
    char want[URL_MAX] = "";
    if (href && strncmp(href, "javascript:", 11) && href[0] != '#') page_resolve(page_url(pg), href, want, sizeof(want));
    if (strcmp(want, t->status)) { strcpy(t->status, want); a->dirty = 1; }
}

const uiapp_class_t app_tfire = {
    "tfire", "tFire", "tfire", 960, 680, 1, sizeof(tf_t),
    tf_init, tf_draw, tf_press, 0, 0, tf_key, tf_wheel, tf_tick, tf_close,
    0, 0, 0,
    tf_hover,
};
