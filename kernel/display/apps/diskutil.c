#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/*
 * Disk Utility, laid out like the Mac's: a sidebar of Internal and External disks, each with
 * its volume underneath; a toolbar of First Aid, Partition, Erase, Restore, Mount/Unmount and
 * Info; and for the selection a large icon, its capacity, a bar of what is used and free, and a
 * table of its details. Erase, First Aid and Info open as sheets. Integer arithmetic only.
 */

#define SIDE_W 236
#define BAR_H 56
#define MAXD 12
#define ROW_H 28

enum { SH_NONE, SH_ERASE, SH_ERASING, SH_AID_ASK, SH_AID_DONE, SH_INFO };

static const char *const fmt_ids[6] = { "fat32", "fat16", "exfat", "ext4", "apfs", "ntfs" };
static const char *const fmt_names[6] = { "MS-DOS (FAT32)", "MS-DOS (FAT16)", "ExFAT", "Linux (ext4)", "APFS", "Windows NT (NTFS)" };

typedef struct { int disk, volume; } row_t;

typedef struct {
    uihost_disk_t d[MAXD];
    int nd;
    row_t rows[MAXD * 2];
    int nrows, sel;                 /* index into rows */
    uint32_t refreshed;
    int sheet;
    char name[24];                  /* the Erase sheet's name field */
    int fmt, fmt_open;
    int waiting_auth;
    char report[1200];
    int result;
    int job;                        /* a background erase or First Aid is running */
    int volumes_only;               /* View > Show Only Volumes */
    char msg[96];
} du_t;

/* ------------------------------------------------------------------ helpers */

/* "8.59 GB": decimal units with two significant decimals, the way the Mac counts */
static void fmt_bytes(char *out, uint64_t b)
{
    static const char *const unit[5] = { " bytes", " KB", " MB", " GB", " TB" };
    int u = 0;
    uint64_t whole = b, frac = 0;
    while (whole >= 1000 && u < 4) { frac = (whole % 1000) / 10; whole /= 1000; u++; }
    out[0] = 0;
    ui_num(out, (int64_t)whole);
    if (u && whole < 100) {
        int l = (int)strlen(out);
        out[l++] = '.';
        out[l++] = (char)('0' + frac / 10);
        if (whole < 10) out[l++] = (char)('0' + frac % 10);
        out[l] = 0;
    }
    strcat(out, unit[u]);
}

static const char *fs_long(const char *fs)
{
    if (!strcmp(fs, "tfsk")) return "tFS";
    if (!strcmp(fs, "fat32")) return "MS-DOS (FAT32)";
    if (!strcmp(fs, "fat16")) return "MS-DOS (FAT16)";
    if (!strcmp(fs, "exfat")) return "ExFAT";
    if (!strncmp(fs, "ext", 3)) return "Linux (ext)";
    if (!strcmp(fs, "apfs")) return "APFS";
    if (!strcmp(fs, "ntfs")) return "Windows NT File System (NTFS)";
    if (!strcmp(fs, "btrfs")) return "Btrfs";
    if (!strcmp(fs, "xfs")) return "XFS";
    if (!strcmp(fs, "zfs")) return "ZFS";
    return fs[0] ? fs : "Unknown";
}

static const char *vol_name(const uihost_disk_t *d) { return d->name[0] ? d->name : "Untitled"; }
static const char *disk_icon(const uihost_disk_t *d, int big) { return d->removable ? (big ? "doc64_disk" : "doc20_disk") : (big ? "doc64_hdd" : "doc20_hdd"); }

static void refresh(du_t *u)
{
    char keep[16] = "";
    int keep_vol = 0;
    if (u->sel >= 0 && u->sel < u->nrows) { strcpy(keep, u->d[u->rows[u->sel].disk].dev); keep_vol = u->rows[u->sel].volume; }
    u->nd = uihost_disks(u->d, MAXD);
    /* Internal first, then External, each disk followed by its volume */
    u->nrows = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < u->nd; i++) {
            if (u->d[i].removable != pass) continue;
            if (!u->volumes_only || !u->d[i].fs[0]) u->rows[u->nrows++] = (row_t){ i, 0 };
            if (u->d[i].fs[0]) u->rows[u->nrows++] = (row_t){ i, 1 };
        }
    u->sel = -1;
    for (int r = 0; r < u->nrows; r++)
        if (!strcmp(u->d[u->rows[r].disk].dev, keep) && u->rows[r].volume == keep_vol) u->sel = r;
    if (u->sel < 0)                       /* the startup volume, as the Mac selects first */
        for (int r = 0; r < u->nrows && u->sel < 0; r++) if (u->rows[r].volume) u->sel = r;
    if (u->sel < 0 && u->nrows) u->sel = 0;
    u->refreshed = uihost_ms();
}

static uihost_disk_t *cur(du_t *u, int *is_vol)
{
    if (u->sel < 0 || u->sel >= u->nrows) return 0;
    if (is_vol) *is_vol = u->rows[u->sel].volume;
    return &u->d[u->rows[u->sel].disk];
}

/* the sidebar's rows, with the section headings in between */
static int row_y(du_t *u, int r)
{
    int y = 38, ext_started = 0;
    for (int i = 0; i <= r; i++) {
        if (!ext_started && u->d[u->rows[i].disk].removable) {
            ext_started = 1;
            if (i > 0) y += 18;
            y += 26;                       /* "External" */
        }
        if (i == r) return y;
        y += ROW_H;
    }
    return y;
}

/* ------------------------------------------------------------------ drawing */

static void tool(gfx_surface_t *s, int x, int y, const char *glyph, const char *label, int enabled)
{
    int w = gfx_text_width(&ui_font_small, label);
    if (w < 24) w = 24;
    gfx_blit_tinted(s, gfx_image(glyph), x + w / 2 - 8, y + 9, enabled ? UI_TEXT : UI_DIM, enabled ? 255 : 110);
    gfx_text(s, &ui_font_small, x, y + 30, label, enabled ? UI_TEXT : UI_DIM, enabled ? 255 : 130);
}

/* toolbar button geometry, shared by drawing and clicks */
static const char *const tool_glyph[6] = { "glyph_stethoscope", "glyph_chart-pie", "glyph_eraser", "glyph_history", "glyph_eject", "glyph_info" };
static const char *tool_label(du_t *u, int i)
{
    static const char *const l[6] = { "First Aid", "Partition", "Erase", "Restore", "Unmount", "Info" };
    int v;
    uihost_disk_t *d = cur(u, &v);
    if (i == 4 && d && !d->mounted) return "Mount";
    return l[i];
}
static int tool_x(du_t *u, int w, int i)
{
    int x = w - 14;
    for (int k = 5; k >= i; k--) {
        int tw = gfx_text_width(&ui_font_small, tool_label(u, k));
        if (tw < 24) tw = 24;
        x -= tw + (k == i ? 0 : 22);
    }
    return x;
}
static int tool_enabled(du_t *u, int i)
{
    int v;
    uihost_disk_t *d = cur(u, &v);
    if (!d) return 0;
    switch (i) {
    case 0: return 1;
    case 2: return !d->startup;
    case 4: return v && d->fs[0] && !d->startup;
    case 5: return 1;
    default: return 0;            /* Partition and Restore are not there yet */
    }
}

static void legend(gfx_surface_t *s, int x, int y, uint32_t c, const char *what, uint64_t bytes)
{
    char b[24];
    fmt_bytes(b, bytes);
    gfx_circle(s, x + 5, y + 8, 5, c, 255);
    gfx_text(s, &ui_font_medium, x + 16, y, what, UI_TEXT, 255);
    gfx_text(s, &ui_font_small, x + 16, y + 18, b, UI_DIM, 255);
}

static void info_row(gfx_surface_t *s, int x, int y, int w, const char *k1, const char *v1, const char *k2, const char *v2, int line)
{
    int half = w / 2;
    gfx_text(s, &ui_font_regular, x + 14, y + 8, k1, UI_TEXT, 255);
    gfx_text(s, &ui_font_regular, x + half - 14 - gfx_text_width(&ui_font_regular, v1), y + 8, v1, UI_DIM, 255);
    gfx_text(s, &ui_font_regular, x + half + 14, y + 8, k2, UI_TEXT, 255);
    gfx_text(s, &ui_font_regular, x + w - 14 - gfx_text_width(&ui_font_regular, v2), y + 8, v2, UI_DIM, 255);
    gfx_fill(s, x + half, y + 6, 1, 22, UI_LINE);
    if (line) gfx_hline(s, x + 12, y + 33, w - 24, UI_LINE, 255);
}

static void sheet_frame(gfx_surface_t *s, int x, int y, int w, int h)
{
    gfx_shadow(s, x, y, w, h, 220);
    gfx_rrect(s, x, y, w, h, 12, ui_theme.menu, 252);
    gfx_rrect_outline(s, x, y, w, h, 12, ui_theme.dark ? 0xFFFFFF : 0x000000, 30);
}

static void draw_text_block(gfx_surface_t *s, int x, int y, int w, const char *t, uint32_t c, int max_lines)
{
    /* wraps at spaces and newlines */
    char line[128];
    int lines = 0;
    const char *p = t;
    while (*p && lines < max_lines) {
        int best = 0, i = 0;
        while (p[i] && p[i] != '\n' && i < 126) {
            line[i] = p[i]; line[i + 1] = 0;
            if (gfx_text_width(&ui_font_regular, line) > w) break;
            if (p[i] == ' ') best = i;
            i++;
        }
        int take = (!p[i] || p[i] == '\n') ? i : (best ? best : i);
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        gfx_text(s, &ui_font_regular, x, y + lines * 19, line, c, 255);
        lines++;
        p += take;
        if (*p == ' ' || *p == '\n') p++;

    }
}

static void du_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    du_t *u = a->st;
    if (!u->refreshed || uihost_ms() - u->refreshed > 2000) refresh(u);

    /* sidebar */
    gfx_fill(s, x, y, SIDE_W, h, UI_SIDEBAR);
    gfx_fill(s, x + SIDE_W, y, 1, h, UI_LINE);
    int any_internal = 0;
    for (int i = 0; i < u->nd; i++) if (!u->d[i].removable) any_internal = 1;
    if (any_internal) gfx_text(s, &ui_font_small, x + 16, y + 16, "Internal", UI_DIM, 255);
    for (int r = 0; r < u->nrows; r++) {
        uihost_disk_t *d = &u->d[u->rows[r].disk];
        int vol = u->rows[r].volume, ry = y + row_y(u, r);
        if (d->removable && (r == 0 || !u->d[u->rows[r - 1].disk].removable))
            gfx_text(s, &ui_font_small, x + 16, ry - 22, "External", UI_DIM, 255);
        if (r == u->sel) gfx_rrect(s, x + 8, ry, SIDE_W - 16, ROW_H - 2, 6, UI_ACCENT, 255);
        uint32_t tc = r == u->sel ? 0xFFFFFF : (vol && !d->mounted ? UI_DIM : UI_TEXT);
        int ix = x + (vol ? 38 : 16);
        if (!vol) gfx_blit_tinted(s, gfx_image("glyph_chevron-right"), x + 2, ry + 6, r == u->sel ? 0xFFFFFF : UI_DIM, 160);
        gfx_blit(s, gfx_image(disk_icon(d, 0)), ix, ry + 3, 255);
        ui_text_fit(s, vol ? &ui_font_regular : &ui_font_medium, ix + 26, ry + 5, SIDE_W - (ix - x) - 26 - 30, vol ? vol_name(d) : d->model, tc);
        if (vol && d->mounted && !d->startup)
            gfx_blit_tinted(s, gfx_image("glyph_eject"), x + SIDE_W - 34, ry + 5, r == u->sel ? 0xFFFFFF : UI_DIM, 255);
    }
    if (!u->nrows) gfx_text(s, &ui_font_regular, x + 16, y + 40, "No disks", UI_DIM, 255);

    /* toolbar */
    int bx = x + SIDE_W + 1, bw = w - SIDE_W - 1;
    gfx_fill(s, bx, y, bw, BAR_H, UI_TOOLBAR);
    gfx_hline(s, bx, y + BAR_H - 1, bw, UI_LINE, 255);
    gfx_text(s, &ui_font_medium, bx + 16, y + 11, "Disk Utility", UI_TEXT, 255);
    int is_vol = 0;
    uihost_disk_t *d = cur(u, &is_vol);
    gfx_text(s, &ui_font_small, bx + 16, y + 30, d ? (is_vol ? vol_name(d) : d->model) : "", UI_DIM, 255);
    for (int i = 0; i < 6; i++) tool(s, bx + tool_x(u, bw, i), y, tool_glyph[i], tool_label(u, i), tool_enabled(u, i));

    /* content */
    int cx = bx, cy = y + BAR_H, cw = bw, ch = h - BAR_H;
    gfx_fill(s, cx, cy, cw, ch, UI_BG);
    if (!d) return;
    int px = cx + 24, py = cy + 20, pw = cw - 48;
    gfx_blit(s, gfx_image(disk_icon(d, 1)), px, py, 255);
    char sub[96] = "", cap[24];
    if (is_vol) {
        strcpy(sub, fs_long(d->fs)); strcat(sub, " Volume \xE2\x80\xA2 "); strcat(sub, d->fs[0] ? d->fs : "unknown");
        gfx_text(s, &ui_font_title, px + 80, py + 8, vol_name(d), UI_TEXT, 255);
        fmt_bytes(cap, d->used_known ? d->total : d->size);
    } else {
        strcpy(sub, d->removable ? "USB External Physical Disk" : "Internal Physical Disk");
        strcat(sub, " \xE2\x80\xA2 "); strcat(sub, d->connection);
        gfx_text(s, &ui_font_title, px + 80, py + 8, d->model, UI_TEXT, 255);
        fmt_bytes(cap, d->size);
    }
    gfx_text(s, &ui_font_regular, px + 80, py + 40, sub, UI_DIM, 255);
    gfx_text(s, &ui_font_title, px + pw - gfx_text_width(&ui_font_title, cap), py + 8, cap, UI_TEXT, 255);
    const char *capl = is_vol ? (d->mounted ? "VOLUME CAPACITY" : "NOT MOUNTED") : "DISK CAPACITY";
    gfx_text(s, &ui_font_small, px + pw - gfx_text_width(&ui_font_small, capl), py + 40, capl, UI_DIM, 255);

    /* the bar: used and free */
    int by = py + 86;
    uint64_t total = is_vol && d->used_known ? d->total : d->size, used = is_vol && d->used_known ? d->used : 0;
    if (!is_vol && d->used_known) used = d->used;
    gfx_rrect(s, px, by, pw, 26, 6, ui_theme.dark ? 0x3A3B42 : 0xE6E6EA, 255);
    gfx_rrect_outline(s, px, by, pw, 26, 6, UI_LINE, 255);
    int uw = total ? (int)((uint64_t)pw * used / total) : 0;
    if (used && uw < 6) uw = 6;
    if (uw) gfx_rrect(s, px, by, uw, 26, 6, is_vol ? 0x5E5CE6 : 0xFF6B57, 255);
    if (uw > 6 && uw < pw - 6) gfx_fill(s, px + uw - 6, by, 6, 26, is_vol ? 0x5E5CE6 : 0xFF6B57);
    if (d->used_known || !is_vol) {
        legend(s, px, by + 38, is_vol ? 0x5E5CE6 : 0xFF6B57, is_vol ? "Used" : (d->mounted ? d->name : "Volumes"), used);
        legend(s, px + 170, by + 38, ui_theme.dark ? 0x55565E : 0xD8D8DE, "Free", total - used);
    } else gfx_text(s, &ui_font_regular, px, by + 40, d->mounted ? "Usage is not known for this file system." : "Mount the volume to see how much of it is used.", UI_DIM, 255);

    /* details */
    int ty = by + 96;
    gfx_rrect(s, px, ty, pw, 4 * 34 + 4, 10, UI_CARD, 255);
    gfx_rrect_outline(s, px, ty, pw, 4 * 34 + 4, 10, UI_LINE, 255);
    char a1[40], a2[40], a3[40];
    if (is_vol) {
        fmt_bytes(a1, d->used_known ? d->total : d->size);
        if (d->used_known) { fmt_bytes(a2, d->total - d->used); fmt_bytes(a3, d->used); } else { strcpy(a2, "\xE2\x80\x94"); strcpy(a3, "\xE2\x80\x94"); }
        info_row(s, px, ty + 2, pw, "Mount Point:", d->mounted ? d->mount : "Not Mounted", "Capacity:", a1, 1);
        info_row(s, px, ty + 36, pw, "Type:", d->startup ? "Startup Volume" : (d->removable ? "USB External Volume" : "Internal Volume"), "Available:", a2, 1);
        info_row(s, px, ty + 70, pw, "Owners:", d->mounted ? "Enabled" : "\xE2\x80\x94", "Used:", a3, 1);
        info_row(s, px, ty + 104, pw, "Connection:", d->connection, "Device:", d->dev, 0);
    } else {
        fmt_bytes(a1, d->size);
        info_row(s, px, ty + 2, pw, "Location:", d->removable ? "External" : "Internal", "Capacity:", a1, 1);
        info_row(s, px, ty + 36, pw, "Connection:", d->connection, "Child count:", d->fs[0] ? "1" : "0", 1);
        info_row(s, px, ty + 70, pw, "Partition Map:", "None (whole disk)", "Type:", "Disk", 1);
        info_row(s, px, ty + 104, pw, "S.M.A.R.T. status:", "Not Supported", "Device:", d->dev, 0);
    }
    if (u->msg[0]) gfx_text(s, &ui_font_small, px, ty + 150, u->msg, UI_ACCENT, 255);

    /* sheets, hanging from the toolbar */
    int sw = 470, sx = cx + (cw - sw) / 2, sy = cy;
    if (u->sheet == SH_ERASE) {
        int sh = u->fmt_open ? 250 + 6 * 24 : 250;
        sheet_frame(s, sx, sy, sw, sh);
        gfx_blit(s, gfx_image(disk_icon(d, 1)), sx + 20, sy + 20, 255);
        char t[80] = "Erase \xE2\x80\x9C";
        strcat(t, is_vol ? vol_name(d) : d->model); strcat(t, "\xE2\x80\x9D?");
        gfx_text(s, &ui_font_medium, sx + 100, sy + 22, t, UI_TEXT, 255);
        draw_text_block(s, sx + 100, sy + 46, sw - 120, "Erasing will permanently erase all data stored on it. Enter a name, choose a format, and click Erase to proceed.", UI_DIM, 3);
        gfx_text(s, &ui_font_regular, sx + 40, sy + 122, "Name:", UI_TEXT, 255);
        gfx_rrect(s, sx + 110, sy + 116, sw - 140, 28, 6, UI_FIELD, 255);
        gfx_rrect_outline(s, sx + 109, sy + 115, sw - 138, 30, 7, UI_ACCENT, 255);
        int nw = gfx_text(s, &ui_font_regular, sx + 118, sy + 121, u->name, UI_TEXT, 255);
        if ((uihost_ms() / 500) % 2 == 0) gfx_fill(s, sx + 119 + nw, sy + 121, 2, 18, UI_TEXT);
        gfx_text(s, &ui_font_regular, sx + 40, sy + 160, "Format:", UI_TEXT, 255);
        gfx_rrect(s, sx + 110, sy + 154, sw - 140, 28, 6, UI_CARD2, 255);
        gfx_text(s, &ui_font_regular, sx + 120, sy + 159, fmt_names[u->fmt], UI_TEXT, 255);
        gfx_blit_tinted(s, gfx_image("glyph_chevron-right"), sx + sw - 54, sy + 160, UI_DIM, 255);
        if (u->fmt_open) {
            for (int i = 0; i < 6; i++) {
                int oy = sy + 186 + i * 24;
                if (i == u->fmt) gfx_rrect(s, sx + 110, oy, sw - 140, 22, 5, UI_ACCENT, 255);
                gfx_text(s, &ui_font_regular, sx + 120, oy + 2, fmt_names[i], i == u->fmt ? 0xFFFFFF : UI_TEXT, 255);
            }
        }
        int byy = sy + sh - 46;
        ui_button(s, sx + sw - 214, byy, 92, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
        ui_button(s, sx + sw - 112, byy, 92, 30, u->waiting_auth ? "Waiting\xE2\x80\xA6" : "Erase", UI_ACCENT, 0xFFFFFF, 0);
    } else if (u->sheet == SH_AID_ASK) {
        sheet_frame(s, sx, sy, sw, 160);
        gfx_blit_tinted(s, gfx_image("glyph_stethoscope"), sx + 30, sy + 30, UI_ACCENT, 255);
        char t[80] = "Run First Aid on \xE2\x80\x9C";
        strcat(t, is_vol ? vol_name(d) : d->model); strcat(t, "\xE2\x80\x9D?");
        gfx_text(s, &ui_font_medium, sx + 70, sy + 22, t, UI_TEXT, 255);
        draw_text_block(s, sx + 70, sy + 46, sw - 90, "First Aid will check the disk for errors. Nothing on it is changed.", UI_DIM, 2);
        ui_button(s, sx + sw - 214, sy + 114, 92, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
        ui_button(s, sx + sw - 112, sy + 114, 92, 30, "Run", UI_ACCENT, 0xFFFFFF, 0);
    } else if (u->sheet == SH_AID_DONE || u->sheet == SH_ERASING) {
        int sh = 340;
        sheet_frame(s, sx, sy, sw, sh);
        int ok = u->result == 0;
        const char *title;
        if (u->job) {
            /* a ring of dots going round while the job runs */
            int ph = (int)(uihost_ms() / 90 % 12);
            for (int k = 0; k < 12; k++) {
                int a = k * 30, al = 40 + ((k - ph + 12) % 12) * 18;
                gfx_circle(s, sx + 40 + gfx_sin(a) * 12 / 4096, sy + 40 - gfx_cos(a) * 12 / 4096, 3, UI_DIM, al > 255 ? 255 : al);
            }
            title = u->sheet == SH_ERASING ? "Erasing\xE2\x80\xA6" : "Running First Aid\xE2\x80\xA6";
        } else {
            gfx_circle(s, sx + 40, sy + 40, 18, ok ? UI_GREEN : (u->result > 0 ? UI_ORANGE : UI_RED), 255);
            gfx_text_center(s, &ui_font_title, sx + 22, sy + 27, 36, ok ? "\xE2\x9C\x93" : "!", 0xFFFFFF, 255);
            title = u->sheet == SH_ERASING ? (ok ? "Erase process is complete." : "Erasing failed.")
                                           : (ok ? "First Aid process is complete." : (u->result > 0 ? "First Aid found problems." : "First Aid failed."));
        }
        gfx_text(s, &ui_font_medium, sx + 76, sy + 30, title, UI_TEXT, 255);
        gfx_rrect(s, sx + 20, sy + 74, sw - 40, sh - 134, 8, UI_FIELD, 255);
        gfx_surface_t c = *s;
        gfx_clip(&c, sx + 24, sy + 78, sw - 48, sh - 142);
        /* the report, last lines kept in view */
        int lines = 0;
        for (const char *p = u->report; *p; p++) if (*p == '\n') lines++;
        int skip = lines > 9 ? lines - 9 : 0;
        const char *p = u->report;
        while (skip && *p) { if (*p == '\n') skip--; p++; }
        int ly = sy + 84;
        while (*p) {
            char line[120];
            int i = 0;
            while (p[i] && p[i] != '\n' && i < 118) { line[i] = p[i]; i++; }
            line[i] = 0;
            gfx_text(&c, &ui_font_small, sx + 32, ly, line, UI_TEXT, 255);
            ly += 20;
            p += i;
            if (*p == '\n') p++;
        }
        ui_button(s, sx + sw - 112, sy + sh - 46, 92, 30, "Done", u->job ? UI_CARD2 : UI_ACCENT, u->job ? UI_DIM : 0xFFFFFF, 0);
    } else if (u->sheet == SH_INFO) {
        int sh = 380;
        sheet_frame(s, sx, sy, sw, sh);
        gfx_blit(s, gfx_image(disk_icon(d, 0)), sx + 20, sy + 20, 255);
        gfx_text(s, &ui_font_medium, sx + 50, sy + 21, is_vol ? vol_name(d) : d->model, UI_TEXT, 255);
        char b1[24], b2[24], b3[24], b4[24];
        fmt_bytes(b1, d->size);
        if (d->used_known) { fmt_bytes(b2, d->total); fmt_bytes(b3, d->used); fmt_bytes(b4, d->total - d->used); }
        else { strcpy(b2, "\xE2\x80\x94"); strcpy(b3, "\xE2\x80\x94"); strcpy(b4, "\xE2\x80\x94"); }
        const char *k[11] = { "Name", "Type", "BSD device name", "Mount point", "File system", "Disk size", "Volume capacity", "Used space", "Available space", "Connection", "Writable" };
        const char *v[11] = { is_vol ? vol_name(d) : d->model, is_vol ? "Volume" : "Disk", d->dev, d->mounted ? d->mount : "Not mounted",
                              fs_long(d->fs), b1, b2, b3, b4, d->connection, d->mounted ? "Yes" : "\xE2\x80\x94" };
        for (int i = 0; i < 11; i++) {
            int ry = sy + 56 + i * 24;
            if (i % 2 == 0) gfx_fill(s, sx + 16, ry - 2, sw - 32, 24, UI_FIELD);
            gfx_text(s, &ui_font_regular, sx + 26, ry + 1, k[i], UI_DIM, 255);
            gfx_text(s, &ui_font_regular, sx + 200, ry + 1, v[i], UI_TEXT, 255);
        }
        ui_button(s, sx + sw - 112, sy + sh - 46, 92, 30, "Close", UI_ACCENT, 0xFFFFFF, 0);
    }
}

/* ------------------------------------------------------------------ actions */

static void do_erase(uiapp_t *a, du_t *u)
{
    uihost_disk_t *d = cur(u, 0);
    if (!d) return;
    u->sheet = SH_ERASING;
    strcpy(u->report, "Erasing \xE2\x80\x9C");
    strcat(u->report, vol_name(d));
    strcat(u->report, "\xE2\x80\x9D as ");
    strcat(u->report, fmt_names[u->fmt]);
    strcat(u->report, "\xE2\x80\xA6\n");
    if (uihost_disk_job(1, d->dev, fmt_ids[u->fmt], u->name) == 0) u->job = 1;
    else { u->result = -1; strcat(u->report, "The disk is busy or the change was not authorised.\n"); }
    a->dirty = 1;
}

static void start_first_aid(du_t *u)
{
    uihost_disk_t *d = cur(u, 0);
    u->sheet = SH_AID_DONE;
    strcpy(u->report, "Running First Aid\xE2\x80\xA6\n");
    if (d && uihost_disk_job(2, d->dev, 0, 0) == 0) u->job = 1;
    else { u->result = -1; strcat(u->report, "The disk is busy with another operation.\n"); }
}

static void du_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    du_t *u = a->st;
    a->dirty = 1;
    u->msg[0] = 0;
    int bw = a->w - SIDE_W - 1, cx = SIDE_W + 1, sw = 470, sx = cx + (bw - sw) / 2, sy = BAR_H;
    if (u->sheet) {
        if (u->sheet == SH_ERASE) {
            int sh = u->fmt_open ? 250 + 6 * 24 : 250;
            if (u->fmt_open) {
                for (int i = 0; i < 6; i++) if (ui_hit(px, py, sx + 110, sy + 186 + i * 24, sw - 140, 22)) u->fmt = i;
                u->fmt_open = 0;
                return;
            }
            if (ui_hit(px, py, sx + 110, sy + 154, sw - 140, 28)) u->fmt_open = 1;
            else if (ui_hit(px, py, sx + sw - 214, sy + sh - 46, 92, 30)) { u->sheet = SH_NONE; u->waiting_auth = 0; }
            else if (ui_hit(px, py, sx + sw - 112, sy + sh - 46, 92, 30) && !u->waiting_auth && u->name[0]) {
                u->waiting_auth = 1;
                uihost_authorize("Disk Utility wants to erase a disk.");
            }
        } else if (u->sheet == SH_AID_ASK) {
            if (ui_hit(px, py, sx + sw - 214, sy + 114, 92, 30)) u->sheet = SH_NONE;
            else if (ui_hit(px, py, sx + sw - 112, sy + 114, 92, 30)) start_first_aid(u);
        } else {
            int sh = u->sheet == SH_INFO ? 380 : 340;
            if (!u->job && ui_hit(px, py, sx + sw - 112, sy + sh - 46, 92, 30)) u->sheet = SH_NONE;
        }
        return;
    }
    if (px < SIDE_W) {
        for (int r = 0; r < u->nrows; r++) {
            int ry = row_y(u, r);
            if (py < ry || py >= ry + ROW_H) continue;
            u->sel = r;
            uihost_disk_t *d = &u->d[u->rows[r].disk];
            if (u->rows[r].volume && d->mounted && !d->startup && px >= SIDE_W - 40) {
                if (uihost_disk_unmount(d->dev) != 0) strcpy(u->msg, "The volume is in use and could not be unmounted.");
                refresh(u);
            }
        }
        return;
    }
    if (py < BAR_H) {
        for (int i = 0; i < 6; i++) {
            int tx = SIDE_W + 1 + tool_x(u, bw, i), tw = gfx_text_width(&ui_font_small, tool_label(u, i));
            if (tw < 24) tw = 24;
            if (!ui_hit(px, py, tx - 6, 2, tw + 12, BAR_H - 4) || !tool_enabled(u, i)) continue;
            int v;
            uihost_disk_t *d = cur(u, &v);
            if (i == 0) u->sheet = SH_AID_ASK;
            else if (i == 2) {
                u->sheet = SH_ERASE;
                u->fmt = 0; u->fmt_open = 0; u->waiting_auth = 0;
                strcpy(u->name, "Untitled");
            } else if (i == 4) {
                int rc = d->mounted ? uihost_disk_unmount(d->dev) : uihost_disk_mount(d->dev);
                if (rc != 0) strcpy(u->msg, d->mounted ? "The volume is in use and could not be unmounted." : "The volume could not be mounted.");
                refresh(u);
            } else if (i == 5) u->sheet = SH_INFO;
        }
    }
}

static void du_key(uiapp_t *a, int k)
{
    du_t *u = a->st;
    a->dirty = 1;
    if (u->sheet == SH_ERASE) {
        int n = (int)strlen(u->name);
        if (k == '\b') { if (n) u->name[n - 1] = 0; }
        else if (k == UI_KEY_ESC) u->sheet = SH_NONE;
        else if (k == '\n') { if (!u->waiting_auth && n) { u->waiting_auth = 1; uihost_authorize("Disk Utility wants to erase a disk."); } }
        else if (k > 32 && k < 127 && k != '/' && n < 11) { u->name[n] = (char)k; u->name[n + 1] = 0; }
        else if (k == ' ' && n && n < 11) { u->name[n] = ' '; u->name[n + 1] = 0; }
        return;
    }
    if (u->sheet && !u->job && (k == UI_KEY_ESC || k == '\n')) { u->sheet = SH_NONE; return; }
    if (k == UI_KEY_UP && u->sel > 0) u->sel--;
    else if (k == UI_KEY_DOWN && u->sel < u->nrows - 1) u->sel++;
}

static int du_tick(uiapp_t *a)
{
    du_t *u = a->st;
    if (u->job) {
        int r = uihost_disk_job_poll(u->report, sizeof(u->report));
        if (r == -2) return (uihost_ms() / 90) % 2;           /* keep the spinner turning */
        u->job = 0;
        u->result = r;
        refresh(u);
        return 1;
    }
    if (u->sheet == SH_ERASE && u->waiting_auth) {
        int r = uihost_authorized();
        if (r == 1) { u->waiting_auth = 0; do_erase(a, u); return 1; }
        if (r == -1) { u->waiting_auth = 0; return 1; }
    }
    static uint32_t blink;
    uint32_t t = uihost_ms() / 500;
    if (u->sheet == SH_ERASE && t != blink) { blink = t; return 1; }
    if (uihost_ms() - u->refreshed > 2000) { refresh(u); return 1; }   /* disks come and go */
    return 0;
}

static void du_init(uiapp_t *a, const char *arg)
{
    (void)arg;
    du_t *u = a->st;
    u->sel = -1;
    refresh(u);
}

/* ------------------------------------------------------------------ the menu bar */

static const char *const du_menus[] = { "File", "Edit", "Images", "View", 0 };

static void menu_action(uiapp_t *a, du_t *u, int act);
enum { M_INFO = 1, M_CLOSE, M_AID, M_PART, M_ERASE, M_RESTORE, M_MOUNT, M_EJECT, M_VOLONLY, M_ALLDEV, M_NONE };
static int menu_act[4][12];

static int du_menu_items(uiapp_t *a, int menu, const char **labels, int *enabled, int max)
{
    du_t *u = a->st;
    int v = 0, n = 0;
    uihost_disk_t *d = cur(u, &v);
    int idle = !u->sheet && !u->job;
#define ITEM(L, E, A) do { if (n < max) { labels[n] = (L); enabled[n] = (E); menu_act[menu][n] = (A); n++; } } while (0)
    switch (menu) {
    case 0:
        ITEM("New Image", 0, M_NONE);
        ITEM("Open Disk Image\xE2\x80\xA6", 0, M_NONE);
        ITEM("-", 0, M_NONE);
        ITEM("Get Info", d && idle, M_INFO);
        ITEM("-", 0, M_NONE);
        ITEM("Close", 1, M_CLOSE);
        break;
    case 1:
        ITEM("Undo", 0, M_NONE);
        ITEM("-", 0, M_NONE);
        ITEM("First Aid\xE2\x80\xA6", d && idle, M_AID);
        ITEM("Partition\xE2\x80\xA6", 0, M_PART);
        ITEM("Erase\xE2\x80\xA6", d && idle && !d->startup, M_ERASE);
        ITEM("Restore\xE2\x80\xA6", 0, M_RESTORE);
        ITEM("-", 0, M_NONE);
        ITEM(d && d->mounted ? "Unmount" : "Mount", d && idle && v && d->fs[0] && !d->startup, M_MOUNT);
        ITEM("Eject", d && idle && d->removable && d->mounted, M_EJECT);
        break;
    case 2:
        ITEM("New Image", 0, M_NONE);
        ITEM("Convert\xE2\x80\xA6", 0, M_NONE);
        ITEM("Scan Image for Restore\xE2\x80\xA6", 0, M_NONE);
        break;
    case 3:
        ITEM(u->volumes_only ? "\xE2\x9C\x93 Show Only Volumes" : "    Show Only Volumes", 1, M_VOLONLY);
        ITEM(u->volumes_only ? "    Show All Devices" : "\xE2\x9C\x93 Show All Devices", 1, M_ALLDEV);
        break;
    }
#undef ITEM
    return n;
}

static void du_menu_pick(uiapp_t *a, int menu, int item)
{
    if (menu < 0 || menu > 3 || item < 0 || item > 11) return;
    menu_action(a, a->st, menu_act[menu][item]);
}

static void menu_action(uiapp_t *a, du_t *u, int act)
{
    int v = 0;
    uihost_disk_t *d = cur(u, &v);
    a->dirty = 1;
    switch (act) {
    case M_INFO: u->sheet = SH_INFO; break;
    case M_CLOSE: uihost_close_window(a); break;
    case M_AID: u->sheet = SH_AID_ASK; break;
    case M_ERASE: u->sheet = SH_ERASE; u->fmt = 0; u->fmt_open = 0; u->waiting_auth = 0; strcpy(u->name, "Untitled"); break;
    case M_MOUNT: case M_EJECT:
        if (d) {
            int rc = d->mounted ? uihost_disk_unmount(d->dev) : uihost_disk_mount(d->dev);
            if (rc != 0) strcpy(u->msg, d->mounted ? "The volume is in use and could not be unmounted." : "The volume could not be mounted.");
            refresh(u);
        }
        break;
    case M_VOLONLY: u->volumes_only = 1; refresh(u); break;
    case M_ALLDEV: u->volumes_only = 0; refresh(u); break;
    }
}

const uiapp_class_t app_diskutil = {
    "diskutil", "Disk Utility", "diskmgr", 900, 560, 1, sizeof(du_t),
    du_init, du_draw, du_press, 0, 0, du_key, 0, du_tick, 0,
    du_menus, du_menu_items, du_menu_pick,
    0,
    0,
};
