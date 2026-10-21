#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"
#include "update.h"

/*
 * What's New in tOS: the release notes of the updates this computer is missing, in a window of
 * their own (System Settings > Software Update > Show Release Notes). App by app, each with its
 * icon, what changed and the commit; then the screenshots, "Before:"/"After:" pairs side by
 * side, and videos playing in place. Scrolls with the wheel or the bar at its edge.
 */

typedef struct {
    int scroll, content, dragging;
} un_t;

#define PAD 28

static void wrap_text(gfx_surface_t *s, const ui_font_t *f, int x, int *y, int w, const char *t, uint32_t c)
{
    char line[200];
    while (*t) {
        int best = 0, i = 0;
        while (t[i] && i < 198) {
            line[i] = t[i]; line[i + 1] = 0;
            if (gfx_text_width(f, line) > w) break;
            if (t[i] == ' ') best = i;
            i++;
        }
        int take = !t[i] ? i : (best ? best : i);
        memcpy(line, t, (size_t)take);
        line[take] = 0;
        gfx_text(s, f, x, *y, line, c, 255);
        *y += f->line_h + 2;
        t += take;
        while (*t == ' ') t++;
    }
}

static void shot(gfx_surface_t *s, const update_shot_t *sh, int x, int y, int w, int *h, uint32_t edge)
{
    *h = 0;
    if (!sh->rgba) return;
    /* a video arrives as its frames one under the other: show the one for now, round and round */
    int video = sh->frames > 1 && sh->frame_ms > 0;
    int fh = video ? sh->h / sh->frames : sh->h, fi = 0;
    if (video) fi = (int)(uihost_ms() / (uint32_t)sh->frame_ms % (uint32_t)sh->frames);
    *h = fh * w / (sh->w ? sh->w : 1);
    gfx_image_t im = { sh->w, fh, sh->rgba + (size_t)fi * sh->w * fh * 4 };
    gfx_blit_scaled(s, &im, x, y, w, *h, 255);
    if (video) {
        /* how far through it is, like a player's bar */
        gfx_fill_alpha(s, x + 10, y + *h - 12, w - 20, 3, 0xFFFFFF, 90);
        gfx_fill_alpha(s, x + 10, y + *h - 12, (w - 20) * (fi + 1) / sh->frames, 3, 0xFFFFFF, 230);
    }
    gfx_rrect_outline(s, x, y, w, *h, 8, edge, 255);
}

static void un_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    un_t *u = a->st;
    const update_info_t *in = uihost_update_status();
    gfx_fill(s, x, y, w, h, UI_BG);
    gfx_surface_t c = *s;
    gfx_clip(&c, x, y, w - 14, h);
    int px = x + PAD, pw = w - 2 * PAD - 8;
    int yy = y + 22 - u->scroll;
    char v[96];
    gfx_blit(&c, gfx_image("rec_install"), px, yy, 255);
    gfx_text(&c, &ui_font_title, px + 80, yy + 6, in->title[0] ? in->title : "What\xE2\x80\x99s New in tOS", UI_TEXT, 255);
    v[0] = 0; strcat(v, "Build "); ui_num(v, in->latest); strcat(v, " \xC2\xB7 "); strcat(v, in->date);
    if (in->behind > 1) { strcat(v, " \xC2\xB7 "); ui_num(v, in->behind); strcat(v, " updates since yours"); }
    gfx_text(&c, &ui_font_regular, px + 80, yy + 38, v, UI_DIM, 255);
    yy += 84;
    gfx_hline(&c, px, yy, pw, UI_LINE, 255);
    yy += 10;
    if (!in->nnotes) { gfx_text(&c, &ui_font_regular, px, yy + 10, "Bug fixes and improvements.", UI_DIM, 255); yy += 40; }
    for (int i = 0; i < in->nnotes; i++) {
        const update_note_t *n = &in->notes[i];
        int first = i == 0 || strcmp(in->notes[i - 1].app, n->app) || in->notes[i - 1].build != n->build;
        int last = i + 1 >= in->nnotes || strcmp(in->notes[i + 1].app, n->app) || in->notes[i + 1].build != n->build;
        if (first) {
            yy += 10;
            char key[32] = "icon48_";
            strcat(key, n->icon);
            const gfx_image_t *ic = gfx_image(key);
            if (!ic) ic = gfx_image("icon48_settings");
            gfx_blit_scaled(&c, ic, px, yy, 32, 32, 255);
            gfx_text(&c, &ui_font_medium, px + 44, yy + 7, n->app, UI_TEXT, 255);
            v[0] = 0; strcat(v, "build "); ui_num(v, n->build);
            gfx_text(&c, &ui_font_small, px + pw - gfx_text_width(&ui_font_small, v), yy + 9, v, UI_DIM, 255);
            yy += 40;
        }
        gfx_circle(&c, px + 49, yy + 8, 2, UI_DIM, 255);
        wrap_text(&c, &ui_font_regular, px + 58, &yy, pw - 62, n->text, UI_TEXT);
        if (last) {
            gfx_text(&c, &ui_font_small, px + 58, yy + 2, n->url, UI_ACCENT, 255);
            yy += 26;
            /* this app's screenshots follow its notes */
            for (int k = 0; k < in->nshots; k++) {
                const update_shot_t *sh = &in->shots[k];
                if (sh->build != n->build || strcmp(sh->app, n->app)) continue;
                if (!strncmp(sh->caption, "After", 5) && k > 0 && !strncmp(in->shots[k - 1].caption, "Before", 6)) continue;
                if (!strncmp(sh->caption, "Before", 6) && k + 1 < in->nshots && !strncmp(in->shots[k + 1].caption, "After", 5)) {
                    int half = (pw - 58 - 12) / 2, tall = 0;
                    for (int t = 0; t < 2; t++) {
                        int sx = px + 58 + t * (half + 12), hh;
                        gfx_text(&c, &ui_font_medium, sx, yy, t ? "AFTER" : "BEFORE", t ? UI_GREEN : UI_DIM, 255);
                        shot(&c, &in->shots[k + t], sx, yy + 22, half, &hh, t ? UI_GREEN : UI_LINE);
                        if (hh > tall) tall = hh;
                    }
                    yy += 22 + tall + 6;
                    int ytop = yy, ymax = yy;
                    for (int t = 0; t < 2; t++) {
                        const char *cap = in->shots[k + t].caption + (t ? 5 : 6);
                        while (*cap == ':' || *cap == ' ') cap++;
                        int y2 = ytop;
                        wrap_text(&c, &ui_font_small, px + 58 + t * (half + 12), &y2, half, cap, UI_DIM);
                        if (y2 > ymax) ymax = y2;
                    }
                    yy = ymax + 14;
                } else {
                    int hh;
                    shot(&c, sh, px + 58, yy, pw - 58, &hh, UI_LINE);
                    yy += hh + 4;
                    wrap_text(&c, &ui_font_small, px + 58, &yy, pw - 58, sh->caption, UI_DIM);
                    yy += 10;
                }
            }
        }
    }
    u->content = yy + u->scroll - y + 20;
    ui_scrollbar(s, x + w - 13, y, h, u->content, u->scroll);
}

static void un_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    un_t *u = a->st;
    if (px >= a->w - 16) { u->dragging = 1; u->scroll = ui_scroll_drag(py, 0, a->h, u->content); a->dirty = 1; }
}

static void un_drag(uiapp_t *a, int px, int py)
{
    (void)px;
    un_t *u = a->st;
    if (u->dragging) { u->scroll = ui_scroll_drag(py, 0, a->h, u->content); a->dirty = 1; }
}

static void un_release(uiapp_t *a, int px, int py) { (void)px; (void)py; ((un_t *)a->st)->dragging = 0; }

static void un_wheel(uiapp_t *a, int d)
{
    un_t *u = a->st;
    u->scroll = ui_scroll_clamp(u->scroll - d * 48, a->h, u->content);
    a->dirty = 1;
}

static void un_key(uiapp_t *a, int k)
{
    un_t *u = a->st;
    if (k == UI_KEY_DOWN) u->scroll += 40;
    else if (k == UI_KEY_UP) u->scroll -= 40;
    else if (k == ' ') u->scroll += a->h - 60;
    u->scroll = ui_scroll_clamp(u->scroll, a->h, u->content);
    a->dirty = 1;
}

static int un_tick(uiapp_t *a)
{
    /* the screenshots arrive a moment after the notes; videos need their next frame */
    static uint32_t last;
    const update_info_t *in = uihost_update_status();
    uint32_t step = 1000;
    for (int i = 0; i < in->nshots; i++)
        if (in->shots[i].rgba && in->shots[i].frames > 1 && (uint32_t)in->shots[i].frame_ms < step) step = (uint32_t)in->shots[i].frame_ms;
    uint32_t t = uihost_ms() / step;
    (void)a;
    if (t != last) { last = t; return 1; }
    return 0;
}

static void un_init(uiapp_t *a, const char *arg) { (void)a; (void)arg; }

const uiapp_class_t app_updatenotes = {
    "updatenotes", "What\xE2\x80\x99s New in tOS", "settings", 700, 560, 1, sizeof(un_t),
    un_init, un_draw, un_press, un_drag, un_release, un_key, un_wheel, un_tick, 0,
    0, 0, 0,
    0,
};
