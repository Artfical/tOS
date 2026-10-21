#include "uiapp.h"
#include "string.h"

extern const uiapp_class_t app_calculator, app_clock, app_2048, app_notepad, app_files, app_viewer,
                           app_activity, app_network, app_paint, app_notes, app_settings, app_diskutil, app_recovery, app_updatenotes, app_tfire;

const uiapp_class_t *const uiapp_classes[] = {
    &app_files, &app_notepad, &app_calculator, &app_paint, &app_viewer, &app_activity, &app_network,
    &app_clock, &app_notes, &app_2048, &app_settings, &app_diskutil, &app_recovery, &app_updatenotes, &app_tfire,
    0,
};
const int uiapp_class_count = (int)(sizeof(uiapp_classes) / sizeof(uiapp_classes[0]));

static const ui_theme_t dark_theme = {
    1, 0x1E1F25, 0x2A2B33, 0x33343D, 0x3A3C46, 0xF2F3F5, 0xA1A4AE,
    0x202127, 0x26272E, 0x24252C, 0x1A1B20, 0x2E4A7A,
    0x2C2E37, 0x24252C, 0xF2F3F5, 0x26272E, 0x101015, 120, 0xF2F3F5,
};
static const ui_theme_t light_theme = {
    0, 0xFFFFFF, 0xF2F2F5, 0xE6E6EB, 0xD9D9DE, 0x1D1D1F, 0x6E6E73,
    0xECECF0, 0xF6F6F8, 0xF2F2F4, 0xFFFFFF, 0xCFE0FC,
    0xE8E8EC, 0xF5F5F7, 0x2B2B2E, 0xF4F4F6, 0xF5F5F7, 170, 0x1D1D1F,
};
ui_theme_t ui_theme = {
    1, 0x1E1F25, 0x2A2B33, 0x33343D, 0x3A3C46, 0xF2F3F5, 0xA1A4AE,
    0x202127, 0x26272E, 0x24252C, 0x1A1B20, 0x2E4A7A,
    0x2C2E37, 0x24252C, 0xF2F3F5, 0x26272E, 0x101015, 120, 0xF2F3F5,
};

void ui_set_dark(int dark) { ui_theme = dark ? dark_theme : light_theme; }

const uiapp_class_t *uiapp_find(const char *id)
{
    for (int i = 0; i < uiapp_class_count; i++)
        if (!strcmp(uiapp_classes[i]->id, id)) return uiapp_classes[i];
    return 0;
}

/* A push button the way macOS draws one: small radius, the default (coloured) button with a
 * soft light from the top, the others white (light) or grey (dark) with a hairline edge and a
 * faint shadow under them. */
void ui_button(gfx_surface_t *s, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int hot)
{
    int r = h >= 30 ? 7 : 6;
    int coloured = bg != ui_theme.card2 && bg != ui_theme.card && bg != ui_theme.field;
    uint32_t face = coloured ? bg : (ui_theme.dark ? 0x5A5A60 : 0xFFFFFF);
    if (hot) face = gfx_mix(face, coloured ? 0xFFFFFF : 0x000000, coloured ? 40 : 18);
    gfx_rrect(s, x, y + 1, w, h, r, 0x000000, ui_theme.dark ? 60 : 22);            /* the shadow under it */
    gfx_rrect(s, x, y, w, h, r, face, 255);
    gfx_rrect(s, x + 1, y + 1, w - 2, h / 2, r - 1, 0xFFFFFF, coloured ? 34 : (ui_theme.dark ? 16 : 0));   /* light from the top */
    if (!coloured) gfx_rrect_outline(s, x, y, w, h, r, 0x000000, ui_theme.dark ? 70 : 34);
    uint32_t tc = coloured ? fg : (fg == ui_theme.dim ? ui_theme.dim : ui_theme.text);
    gfx_text_center(s, &ui_font_regular, x, y + (h - 17) / 2, w, label, tc, 255);
}

/* Text that may be longer than the room it has: cut at the last whole character that fits and
 * end it with an ellipsis, instead of running under whatever is next to it. */
void ui_text_fit(gfx_surface_t *s, const ui_font_t *f, int x, int y, int maxw, const char *t, uint32_t c)
{
    if (maxw <= 0) return;
    if (gfx_text_width(f, t) <= maxw) { gfx_text(s, f, x, y, t, c, 255); return; }
    char line[256];
    int ew = gfx_text_width(f, "\xE2\x80\xA6"), n = 0, keep = 0;
    while (t[n] && n < 250) {
        int len = 1;
        while ((t[n + len] & 0xC0) == 0x80) len++;          /* never split a UTF-8 character */
        memcpy(line, t, (size_t)(n + len));
        line[n + len] = 0;
        if (gfx_text_width(f, line) + ew > maxw) break;
        n += len;
        keep = n;
    }
    while (keep > 0 && t[keep - 1] == ' ') keep--;
    memcpy(line, t, (size_t)keep);
    memcpy(line + keep, "\xE2\x80\xA6", 4);
    gfx_text(s, f, x, y, line, c, 255);
}

int ui_hit(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

/* the scroll bar: a thin rounded thumb over a faint track, like macOS's when scrolling */
void ui_scrollbar(gfx_surface_t *s, int x, int y, int view, int content, int scroll)
{
    if (content <= view || view < 20) return;
    int th = view * view / content;
    if (th < 28) th = 28;
    int ty = y + (int)((int64_t)(view - th) * scroll / (content - view));
    gfx_rrect(s, x, y + 2, 10, view - 4, 5, ui_theme.dark ? 0xFFFFFF : 0x000000, 18);
    gfx_rrect(s, x + 2, ty + 2, 6, th - 4, 3, ui_theme.dark ? 0xFFFFFF : 0x000000, ui_theme.dark ? 110 : 90);
}

int ui_scroll_drag(int py, int y, int view, int content)
{
    if (content <= view) return 0;
    int th = view * view / content;
    if (th < 28) th = 28;
    int pos = py - y - th / 2;
    return ui_scroll_clamp((int)((int64_t)pos * (content - view) / (view - th > 0 ? view - th : 1)), view, content);
}

int ui_scroll_clamp(int scroll, int view, int content)
{
    if (scroll > content - view) scroll = content - view;
    if (scroll < 0) scroll = 0;
    return scroll;
}

void ui_num(char *out, int64_t v)
{
    char t[24];
    int n = 0, o = (int)strlen(out);
    if (v < 0) { out[o++] = '-'; v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[o++] = t[--n];
    out[o] = 0;
}

void ui_size(char *out, uint32_t b)
{
    if (b < 1024) { ui_num(out, b); strcat(out, " B"); }
    else if (b < 1024 * 1024) { ui_num(out, (b + 512) / 1024); strcat(out, " KB"); }
    else {
        ui_num(out, b / (1024 * 1024));
        uint32_t frac = (b % (1024 * 1024)) * 10 / (1024 * 1024);
        strcat(out, ".");
        ui_num(out, frac);
        strcat(out, " MB");
    }
}

/* ---- paths ---- */

void ui_path_join(char *out, const char *dir, const char *name)
{
    strcpy(out, dir);
    int n = (int)strlen(out);
    if (n == 0 || out[n - 1] != '/') strcat(out, "/");
    strcat(out, name);
}

const char *ui_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* ---- the file chooser ---- */

static void dlg_load(ui_filedlg_t *d)
{
    d->n = uihost_list(d->dir, d->ents, 96);
    if (d->n < 0) d->n = 0;
    int keep = 0;                                  /* hide dot files */
    for (int i = 0; i < d->n; i++) if (d->ents[i].name[0] != '.') d->ents[keep++] = d->ents[i];
    d->n = keep;
    /* folders first, then files, each alphabetically */
    for (int i = 1; i < d->n; i++) {
        uihost_entry_t t = d->ents[i];
        int j = i - 1;
        while (j >= 0 && (d->ents[j].is_dir < t.is_dir ||
               (d->ents[j].is_dir == t.is_dir && strcmp(d->ents[j].name, t.name) > 0))) {
            d->ents[j + 1] = d->ents[j];
            j--;
        }
        d->ents[j + 1] = t;
    }
    d->scroll = 0;
    d->sel = -1;
}

void ui_filedlg_open(ui_filedlg_t *d, int saving, const char *start_dir, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->active = 1;
    d->saving = saving;
    strncpy(d->dir, start_dir && start_dir[0] ? start_dir : uihost_home(), sizeof(d->dir) - 1);
    if (name) strncpy(d->name, name, sizeof(d->name) - 1);
    dlg_load(d);
}

#define DLG_W 460
#define DLG_H 380
#define DLG_ROW 26

static void dlg_frame(int w, int h, int *dx, int *dy)
{
    *dx = (w - DLG_W) / 2; *dy = (h - DLG_H) / 2;
    if (*dx < 4) *dx = 4;
    if (*dy < 4) *dy = 4;
}

void ui_filedlg_draw(ui_filedlg_t *d, gfx_surface_t *s, int x, int y, int w, int h)
{
    if (!d->active) return;
    gfx_fill_alpha(s, x, y, w, h, 0x000000, 120);
    int dx, dy;
    dlg_frame(w, h, &dx, &dy);
    dx += x; dy += y;
    gfx_shadow(s, dx, dy, DLG_W, DLG_H, 220);
    gfx_rrect(s, dx, dy, DLG_W, DLG_H, 12, UI_TOOLBAR, 255);
    gfx_text(s, &ui_font_medium, dx + 18, dy + 14, d->saving ? "Save As" : "Open", UI_TEXT, 255);
    /* the folder, with an "up" button */
    ui_button(s, dx + 18, dy + 42, 34, 26, "\xE2\x86\x90", UI_CARD2, UI_TEXT, 0);
    ui_text_fit(s, &ui_font_regular, dx + 62, dy + 46, DLG_W - 82, d->dir, UI_DIM);
    /* the listing */
    int ly = dy + 78, rows = (DLG_H - 78 - 96) / DLG_ROW;
    gfx_rrect(s, dx + 18, ly, DLG_W - 36, rows * DLG_ROW + 8, 8, UI_FIELD, 255);
    for (int i = 0; i < rows && d->scroll + i < d->n; i++) {
        uihost_entry_t *e = &d->ents[d->scroll + i];
        int ry = ly + 4 + i * DLG_ROW;
        if (d->scroll + i == d->sel) gfx_rrect(s, dx + 22, ry, DLG_W - 44, DLG_ROW, 6, UI_ACCENT, 255);
        gfx_blit(s, gfx_image(e->is_dir ? "icon20_files" : "icon20_notepad"), dx + 28, ry + 3, 255);
        gfx_text(s, &ui_font_regular, dx + 56, ry + 4, e->name, UI_TEXT, 255);
        if (!e->is_dir) {
            char sz[16] = "";
            ui_size(sz, e->size);
            gfx_text(s, &ui_font_small, dx + DLG_W - 40 - gfx_text_width(&ui_font_small, sz), ry + 6, sz, UI_DIM, 255);
        }
    }
    if (!d->n) gfx_text(s, &ui_font_regular, dx + 32, ly + 12, "(empty folder)", UI_DIM, 255);
    /* the name field and buttons */
    int fy = dy + DLG_H - 84;
    gfx_text(s, &ui_font_small, dx + 18, fy + 6, "Name", UI_DIM, 255);
    gfx_rrect(s, dx + 64, fy, DLG_W - 82, 30, 7, UI_FIELD, 255);
    gfx_rrect_outline(s, dx + 64, fy, DLG_W - 82, 30, 7, UI_ACCENT, 200);
    int tw = gfx_text(s, &ui_font_regular, dx + 74, fy + 6, d->name, UI_TEXT, 255);
    gfx_fill(s, dx + 75 + tw, fy + 6, 2, 18, UI_ACCENT);
    ui_button(s, dx + DLG_W - 220, dy + DLG_H - 44, 96, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
    ui_button(s, dx + DLG_W - 114, dy + DLG_H - 44, 96, 30, d->saving ? "Save" : "Open", UI_ACCENT, 0xFFFFFF, 0);
}

static void dlg_up(ui_filedlg_t *d)
{
    int n = (int)strlen(d->dir);
    while (n > 1 && d->dir[n - 1] == '/') d->dir[--n] = 0;
    while (n > 1 && d->dir[n - 1] != '/') d->dir[--n] = 0;
    if (n > 1) d->dir[n - 1] = 0;
    if (!d->dir[0]) strcpy(d->dir, "/");
    dlg_load(d);
}

static int dlg_confirm(ui_filedlg_t *d)
{
    if (!d->name[0]) return 0;
    if (d->name[0] == '/') strncpy(d->result, d->name, sizeof(d->result) - 1);
    else ui_path_join(d->result, d->dir, d->name);
    d->active = 0;
    return 1;
}

int ui_filedlg_press(ui_filedlg_t *d, int px, int py, int w, int h)
{
    if (!d->active) return 0;
    int dx, dy;
    dlg_frame(w, h, &dx, &dy);
    px -= dx; py -= dy;
    if (ui_hit(px, py, 18, 42, 34, 26)) { dlg_up(d); return 0; }
    if (ui_hit(px, py, DLG_W - 220, DLG_H - 44, 96, 30)) { d->active = 0; return -1; }
    if (ui_hit(px, py, DLG_W - 114, DLG_H - 44, 96, 30)) return dlg_confirm(d);
    int ly = 78, rows = (DLG_H - 78 - 96) / DLG_ROW;
    if (ui_hit(px, py, 18, ly, DLG_W - 36, rows * DLG_ROW + 8)) {
        int i = d->scroll + (py - ly - 4) / DLG_ROW;
        if (i < 0 || i >= d->n) return 0;
        uint32_t now = uihost_ms();
        int dbl = (i == d->sel && now - d->last_click_ms < 450);
        d->last_click_ms = now;
        d->sel = i;
        if (d->ents[i].is_dir) {
            if (dbl) { char nd[160]; ui_path_join(nd, d->dir, d->ents[i].name); strncpy(d->dir, nd, sizeof(d->dir) - 1); dlg_load(d); }
        } else {
            strncpy(d->name, d->ents[i].name, sizeof(d->name) - 1);
            if (dbl) return dlg_confirm(d);
        }
    }
    return 0;
}

int ui_filedlg_key(ui_filedlg_t *d, int k)
{
    if (!d->active) return 0;
    int n = (int)strlen(d->name);
    if (k == UI_KEY_ESC) { d->active = 0; return -1; }
    if (k == '\n') return dlg_confirm(d);
    if (k == '\b') { if (n) d->name[n - 1] = 0; return 0; }
    if (k == UI_KEY_DOWN && d->sel + 1 < d->n) { d->sel++; if (!d->ents[d->sel].is_dir) strcpy(d->name, d->ents[d->sel].name); }
    else if (k == UI_KEY_UP && d->sel > 0) { d->sel--; if (!d->ents[d->sel].is_dir) strcpy(d->name, d->ents[d->sel].name); }
    else if (k >= 32 && k < 127 && n < (int)sizeof(d->name) - 1) { d->name[n] = (char)k; d->name[n + 1] = 0; }
    int rows = (DLG_H - 78 - 96) / DLG_ROW;
    if (d->sel >= d->scroll + rows) d->scroll = d->sel - rows + 1;
    if (d->sel >= 0 && d->sel < d->scroll) d->scroll = d->sel;
    return 0;
}
