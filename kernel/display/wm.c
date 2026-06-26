#include "wm.h"
#include "terminal.h"
#include "mouse.h"
#include "scheduler.h"
#include "shell.h"
#include "string.h"
#include "cmos.h"
#include "net.h"
#include "notepad.h"
#include "clock.h"

#define VGA_W 80
#define VGA_H 25
#define MENU_ROW 0
#define DOCK_ROW (VGA_H - 1)

#define MAX_WINDOWS 6
#define NORMAL_W 60
#define NORMAL_H 16

#define WIN_KIND_TERMINAL 0
#define WIN_KIND_NOTEPAD 1
#define WIN_KIND_CLOCK 2

static uint16_t *const VGA_MEM = (uint16_t *)0xB8000;
static uint16_t backbuffer[VGA_W * VGA_H];

typedef struct {
    term_surface_t surface;
    int open;
    int minimized;
    int maximized;
    int pid;
    int z;
    int kind;
    char title[40];
    char initial_cmd[64];
    int x0, y0, w0, h0;
    int tb_x0, tb_x1;
} window_t;

static window_t windows[MAX_WINDOWS];
static window_t *wm_focused = NULL;
static window_t *pending_window;
static int next_z = 1;

/* Top menu bar: T (apple-logo stand-in) + File/Edit/View/Label/Special */
enum { MENU_NONE = 0, MENU_T, MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_LABEL, MENU_SPECIAL };
static int active_menu = MENU_NONE;

static int t_x0, t_x1;
static int file_x0, file_x1;
static int edit_x0, edit_x1;
static int view_x0, view_x1;
static int label_x0, label_x1;
static int special_x0, special_x1;

#define MENU_MAX_ITEMS 64
static int menu_x0[MENU_MAX_ITEMS];
static int menu_y0[MENU_MAX_ITEMS];
static int menu_x1[MENU_MAX_ITEMS];
static int menu_count = 0;
static const char *menu_names[MENU_MAX_ITEMS];
static int menu_is_app[MENU_MAX_ITEMS];
static int menu_disabled[MENU_MAX_ITEMS];

/* Rainbow palette for the "T" logo, cycled over time like the old Apple logo */
static const uint8_t rainbow_colors[6] = {
    VGA_LIGHT_GREEN, VGA_LIGHT_BROWN, VGA_LIGHT_RED, VGA_RED, VGA_LIGHT_MAGENTA, VGA_LIGHT_BLUE
};

static uint8_t mk_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }
static uint16_t mk_cell(char c, uint8_t color) { return (uint16_t)(unsigned char)c | ((uint16_t)color << 8); }

static void vga_put(int x, int y, char c, uint8_t color)
{
    if (x < 0 || x >= VGA_W || y < 0 || y >= VGA_H) return;
    backbuffer[y * VGA_W + x] = mk_cell(c, color);
}

static void vga_fill_rect(int x0, int y0, int w, int h, char c, uint8_t color)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            vga_put(x, y, c, color);
}

static void vga_text(int x, int y, const char *s, uint8_t color)
{
    int i = 0;
    while (s[i]) { vga_put(x + i, y, s[i], color); i++; }
}

int wm_current_task_has_focus(void)
{
    void *ud = task_get_userdata();
    if (!ud) return 1;
    return ud == (void *)wm_focused;
}

static void window_geom(window_t *w, int *x0, int *y0, int *w0, int *h0)
{
    int slot = (int)(w - windows);
    if (w->maximized) {
        *x0 = 0; *y0 = MENU_ROW + 1; *w0 = VGA_W; *h0 = VGA_H - 2;
    } else {
        *x0 = 4 + (slot % 4) * 3;
        *y0 = 2 + (slot % 3) * 2;
        *w0 = NORMAL_W;
        *h0 = NORMAL_H;
    }
}

static int fmt_uint(char *buf, unsigned int v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int fmt_uint02(char *buf, unsigned int v)
{
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + (v % 10);
    return 2;
}

static int wm_find_free_slot(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].open) return i;
    return -1;
}

static void window_task_entry(void)
{
    window_t *w = pending_window;
    task_set_userdata(w);
    if (w->kind == WIN_KIND_NOTEPAD)
        notepad_run();
    else if (w->kind == WIN_KIND_CLOCK)
        clock_run();
    else
        shell_run_windowed(w->initial_cmd);
}

static void wm_focus_window(window_t *w)
{
    w->minimized = 0;
    w->z = next_z++;
    wm_focused = w;
}

static void wm_close_window(window_t *w)
{
    task_kill((uint32_t)w->pid);
    w->open = 0;
    w->minimized = 0;
    w->maximized = 0;
    if (wm_focused == w) wm_focused = NULL;
}

static void wm_open_window(const char *cmd)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_TERMINAL;

    strncpy(w->initial_cmd, cmd, sizeof(w->initial_cmd) - 1);
    w->initial_cmd[sizeof(w->initial_cmd) - 1] = 0;

    if (cmd[0]) {
        const char *prefix = "Terminal - ";
        int k = 0;
        while (prefix[k] && k < (int)sizeof(w->title) - 1) { w->title[k] = prefix[k]; k++; }
        int j = 0;
        while (cmd[j] && k < (int)sizeof(w->title) - 1) { w->title[k++] = cmd[j++]; }
        w->title[k] = 0;
    } else {
        strcpy(w->title, "Terminal");
    }

    window_geom(w, &w->x0, &w->y0, &w->w0, &w->h0);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_notepad(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_NOTEPAD;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Notepad");

    window_geom(w, &w->x0, &w->y0, &w->w0, &w->h0);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_clock(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_CLOCK;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Clock");

    window_geom(w, &w->x0, &w->y0, &w->w0, &w->h0);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void draw_window(window_t *w)
{
    int x0, y0, w0, h0;
    window_geom(w, &x0, &y0, &w0, &h0);

    int is_active = (w == wm_focused);
    uint8_t title_color = is_active ? mk_color(VGA_BLACK, VGA_WHITE) : mk_color(VGA_BLACK, VGA_LIGHT_GREY);

    vga_fill_rect(x0, y0, w0, 1, ' ', title_color);

    /* Mac-style pinstripes filling the active title bar */
    if (is_active) {
        for (int x = x0; x < x0 + w0; x += 2)
            vga_put(x, y0, '-', mk_color(VGA_LIGHT_GREY, VGA_WHITE));
    }

    /* close box on the left, like classic Mac OS */
    vga_put(x0 + 1, y0, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    vga_fill_rect(x0 + 1, y0, 1, 1, 0xFE, title_color);

    int title_len = (int)strlen(w->title);
    int title_x = x0 + (w0 - title_len) / 2;
    if (title_x < x0 + 3) title_x = x0 + 3;
    vga_fill_rect(title_x - 1, y0, title_len + 2, 1, ' ', title_color);
    vga_text(title_x, y0, w->title, title_color);

    int gx = x0 + w0 - 4;
    vga_put(gx, y0, '_', title_color);
    vga_put(gx + 1, y0, 0xFE, title_color);
    vga_put(gx + 2, y0, 'X', mk_color(VGA_WHITE, VGA_RED));

    int content_h = h0 - 1;
    for (int row = 0; row < content_h; row++) {
        for (int col = 0; col < w0; col++) {
            uint16_t cell;
            if (row < TERM_SURFACE_H && col < TERM_SURFACE_W)
                cell = w->surface.cells[row * TERM_SURFACE_W + col];
            else
                cell = mk_cell(' ', mk_color(VGA_LIGHT_GREY, VGA_BLACK));
            int vx = x0 + col, vy = y0 + 1 + row;
            if (vx >= 0 && vx < VGA_W && vy >= 0 && vy < VGA_H - 1)
                backbuffer[vy * VGA_W + vx] = cell;
        }
    }

    w->x0 = x0; w->y0 = y0; w->w0 = w0; w->h0 = h0;
}

static int dropdown_hit(int cx, int cy, int *out_index)
{
    for (int i = 0; i < menu_count; i++) {
        if (menu_disabled[i]) continue;
        if (cx >= menu_x0[i] && cx <= menu_x1[i] && cy == menu_y0[i]) { *out_index = i; return 1; }
    }
    return 0;
}

static void build_menu_items(int which)
{
    menu_count = 0;
    if (which == MENU_T) {
        menu_names[menu_count] = "About This Computer..."; menu_is_app[menu_count] = -1; menu_disabled[menu_count] = 1;
        menu_count++;
        menu_names[menu_count] = "Notepad"; menu_is_app[menu_count] = 1; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Terminal"; menu_is_app[menu_count] = 3; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Clock"; menu_is_app[menu_count] = 2; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Shut Down"; menu_is_app[menu_count] = -2; menu_disabled[menu_count] = 0;
        menu_count++;
    } else if (which == MENU_FILE) {
        menu_names[menu_count] = "New Window"; menu_is_app[menu_count] = -3; menu_disabled[menu_count] = 0; menu_count++;
        menu_names[menu_count] = "Close Window"; menu_is_app[menu_count] = -4; menu_disabled[menu_count] = 0; menu_count++;
    } else {
        menu_names[menu_count] = "(no items)"; menu_is_app[menu_count] = -1; menu_disabled[menu_count] = 1; menu_count++;
    }
}

static void draw_divider(int x0, int y, int w)
{
    vga_fill_rect(x0, y, w, 1, 0xC4, mk_color(VGA_LIGHT_GREY, VGA_WHITE));
}

/* The T menu (apple-menu equivalent) lists every shell command plus Notepad,
   far too many to fit one-per-row in 25 text rows, so the app list is laid
   out as a grid (like the original Start menu) while the header and
   Shut Down stay as full-width rows above/below it. */
static void draw_t_menu(int anchor_x0)
{
    build_menu_items(MENU_T);

    int header_idx = 0;
    int shutdown_idx = menu_count - 1;
    int apps_start = 1;
    int apps_count = menu_count - 2;

    int cols = 4;
    int item_w = 11;
    int rows = (apps_count + cols - 1) / cols;
    int grid_w = cols * item_w;

    int header_w = (int)strlen(menu_names[header_idx]) + 2;
    int shutdown_w = (int)strlen(menu_names[shutdown_idx]) + 2;
    int total_w = grid_w;
    if (header_w > total_w) total_w = header_w;
    if (shutdown_w > total_w) total_w = shutdown_w;

    int mx0 = anchor_x0;
    if (mx0 + total_w >= VGA_W) mx0 = VGA_W - total_w;
    if (mx0 < 0) mx0 = 0;
    int my0 = MENU_ROW + 1;
    int menu_h = 1 + 1 + rows + 1 + 1;
    if (my0 + menu_h > DOCK_ROW) menu_h = DOCK_ROW - my0;

    vga_fill_rect(mx0, my0, total_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    for (int y = my0; y < my0 + menu_h; y++) vga_put(mx0 + total_w, y, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
    vga_fill_rect(mx0, my0 + menu_h, total_w + 1, 1, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));

    vga_text(mx0 + 1, my0, menu_names[header_idx], mk_color(VGA_LIGHT_GREY, VGA_WHITE));
    menu_x0[header_idx] = mx0; menu_y0[header_idx] = my0; menu_x1[header_idx] = mx0 + total_w - 1;

    draw_divider(mx0, my0 + 1, total_w);

    for (int i = 0; i < apps_count; i++) {
        int idx = apps_start + i;
        int col = i % cols;
        int row = i / cols;
        int ix = mx0 + 1 + col * item_w;
        int iy = my0 + 2 + row;
        if (iy >= my0 + menu_h - 2) continue;
        vga_text(ix, iy, menu_names[idx], mk_color(VGA_BLACK, VGA_WHITE));
        menu_x0[idx] = ix; menu_y0[idx] = iy; menu_x1[idx] = ix + item_w - 1;
    }

    int shutdown_y = my0 + 2 + rows + 1;
    draw_divider(mx0, shutdown_y - 1, total_w);
    vga_text(mx0 + 1, shutdown_y, menu_names[shutdown_idx], mk_color(VGA_BLACK, VGA_WHITE));
    menu_x0[shutdown_idx] = mx0; menu_y0[shutdown_idx] = shutdown_y; menu_x1[shutdown_idx] = mx0 + total_w - 1;
}

static void draw_simple_menu(int which, int anchor_x0)
{
    build_menu_items(which);

    int item_w = 0;
    for (int i = 0; i < menu_count; i++) {
        int len = (int)strlen(menu_names[i]) + 2;
        if (len > item_w) item_w = len;
    }
    if (item_w < 14) item_w = 14;

    int mx0 = anchor_x0;
    if (mx0 + item_w >= VGA_W) mx0 = VGA_W - item_w;
    if (mx0 < 0) mx0 = 0;
    int my0 = MENU_ROW + 1;
    int menu_h = menu_count + 1;

    vga_fill_rect(mx0, my0, item_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    for (int y = my0; y < my0 + menu_h; y++) vga_put(mx0 + item_w, y, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
    vga_fill_rect(mx0, my0 + menu_h, item_w + 1, 1, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < menu_count; i++) {
        int ix = mx0 + 1;
        int iy = my0 + i;
        uint8_t c = menu_disabled[i] ? mk_color(VGA_LIGHT_GREY, VGA_WHITE) : mk_color(VGA_BLACK, VGA_WHITE);
        vga_text(ix, iy, menu_names[i], c);
        menu_x0[i] = mx0; menu_y0[i] = iy; menu_x1[i] = mx0 + item_w - 1;
    }
}

static void draw_dropdown_menu(int which, int anchor_x0, int anchor_x1)
{
    (void)anchor_x1;
    if (which == MENU_T) draw_t_menu(anchor_x0);
    else draw_simple_menu(which, anchor_x0);
}

static void draw_menu_bar(void)
{
    vga_fill_rect(0, MENU_ROW, VGA_W, 1, ' ', mk_color(VGA_BLACK, VGA_WHITE));

    /* The "T" stands in for the old rainbow Apple logo; cycle through the
       classic stripe colors since one VGA text cell can only hold one color
       at a time. */
    uint32_t ticks = task_get_ticks();
    uint8_t t_color = rainbow_colors[(ticks / 8) % 6];
    t_x0 = 1; t_x1 = 2;
    vga_put(t_x0, MENU_ROW, 'T', mk_color(t_color, VGA_WHITE));

    int x = 4;
    uint8_t menu_color = mk_color(VGA_BLACK, VGA_WHITE);
    uint8_t menu_active_color = mk_color(VGA_WHITE, VGA_BLACK);

    const char *labels[5] = { "File", "Edit", "View", "Label", "Special" };
    int active_ids[5] = { MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_LABEL, MENU_SPECIAL };
    int *xs0[5] = { &file_x0, &edit_x0, &view_x0, &label_x0, &special_x0 };
    int *xs1[5] = { &file_x1, &edit_x1, &view_x1, &label_x1, &special_x1 };

    for (int i = 0; i < 5; i++) {
        int len = (int)strlen(labels[i]);
        uint8_t c = (active_menu == active_ids[i]) ? menu_active_color : menu_color;
        if (active_menu == active_ids[i]) vga_fill_rect(x - 1, MENU_ROW, len + 2, 1, ' ', c);
        vga_text(x, MENU_ROW, labels[i], c);
        *xs0[i] = x - 1; *xs1[i] = x + len;
        x += len + 3;
    }

    if (active_menu == MENU_T) vga_fill_rect(t_x0 - 1, MENU_ROW, 4, 1, ' ', menu_active_color);

    cmos_time_t t;
    cmos_get_time(&t);

    char netbuf[24];
    if (net_ip == 0) {
        strcpy(netbuf, "NET: --");
    } else {
        int k = 0;
        const char *pfx = "NET: ";
        while (pfx[k]) { netbuf[k] = pfx[k]; k++; }
        unsigned int o1 = net_ip & 0xFF;
        unsigned int o2 = (net_ip >> 8) & 0xFF;
        unsigned int o3 = (net_ip >> 16) & 0xFF;
        unsigned int o4 = (net_ip >> 24) & 0xFF;
        k += fmt_uint(netbuf + k, o1); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o2); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o3); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o4);
        netbuf[k] = 0;
    }

    char right[40];
    int rk = 0;
    int nk = 0;
    while (netbuf[nk]) right[rk++] = netbuf[nk++];
    right[rk++] = ' '; right[rk++] = ' '; right[rk++] = 0xF8; right[rk++] = ' ';
    rk += fmt_uint02(right + rk, (unsigned int)t.hour);
    right[rk++] = ':';
    rk += fmt_uint02(right + rk, (unsigned int)t.minute);
    right[rk] = 0;

    int rlen = (int)strlen(right);
    vga_text(VGA_W - rlen - 1, MENU_ROW, right, mk_color(VGA_BLACK, VGA_WHITE));
}

static void draw_dock(void)
{
    vga_fill_rect(0, DOCK_ROW, VGA_W, 1, ' ', mk_color(VGA_BLACK, VGA_LIGHT_GREY));

    int x = 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open) continue;
        char buf[12];
        int k = 0;
        while (w->title[k] && k < 10) { buf[k] = w->title[k]; k++; }
        buf[k] = 0;
        int len = (int)strlen(buf) + 2;
        if (x + len >= VGA_W - 1) break;
        uint8_t c = (w == wm_focused && !w->minimized) ? mk_color(VGA_BLACK, VGA_WHITE) : mk_color(VGA_BLACK, VGA_LIGHT_GREY);
        vga_put(x, DOCK_ROW, '[', c);
        vga_text(x + 1, DOCK_ROW, buf, c);
        vga_put(x + 1 + (int)strlen(buf), DOCK_ROW, ']', c);
        w->tb_x0 = x; w->tb_x1 = x + len - 1;
        x += len + 1;
    }
}

/* Decorative trash can in the bottom-right corner of the desktop, like the
   classic Mac OS Finder desktop. Drawn as a small black-on-white pixel icon
   (handle, lid, ridged body, base) instead of a flat colored box, so it
   reads as a bin rather than a solid rectangle. Purely cosmetic; not wired
   to file deletion. */
static void draw_trash(void)
{
    int x = VGA_W - 9;
    int y = DOCK_ROW - 6;
    uint8_t line = mk_color(VGA_BLACK, VGA_WHITE);
    uint8_t shade = mk_color(VGA_DARK_GREY, VGA_WHITE);

    vga_put(x + 2, y, 0xDA, line);
    vga_put(x + 3, y, 0xC4, line);
    vga_put(x + 4, y, 0xBF, line);

    vga_fill_rect(x, y + 1, 7, 1, 0xC4, line);

    for (int r = 0; r < 2; r++) {
        vga_put(x + 1, y + 2 + r, 0xB3, line);
        for (int i = 0; i < 3; i++)
            vga_put(x + 2 + i, y + 2 + r, (r + i) % 2 ? 0xB1 : 0xB2, shade);
        vga_put(x + 5, y + 2 + r, 0xB3, line);
    }

    vga_put(x + 1, y + 4, 0xC0, line);
    vga_fill_rect(x + 2, y + 4, 3, 1, 0xC4, line);
    vga_put(x + 5, y + 4, 0xD9, line);

    vga_text(x + 1, y + 5, "Trash", mk_color(VGA_BLACK, VGA_WHITE));
}

#define MOUSE_CURSOR_GLYPH 0x10 /* CP437 solid right-pointing arrow */

static void draw_cursor(void)
{
    int mx, my; uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    (void)btns;
    if (mx < 0 || mx >= VGA_W || my < 0 || my >= VGA_H) return;
    uint16_t cell = backbuffer[my * VGA_W + mx];
    uint8_t bg = (cell >> 12) & 0x0F;
    uint8_t cursor_fg = (bg == VGA_BLACK) ? VGA_WHITE : VGA_BLACK;
    backbuffer[my * VGA_W + mx] = mk_cell((char)MOUSE_CURSOR_GLYPH, mk_color(bg, cursor_fg));
}

#define FRAME_INTERVAL_TICKS 1
static uint32_t last_draw_tick = 0;

static void handle_menu_click(int idx)
{
    if (active_menu == MENU_T) {
        if (menu_is_app[idx] == -2) {
            /* Shut Down: nothing destructive to wire up yet, just close the menu */
        } else if (menu_is_app[idx] == 1) {
            wm_open_notepad();
        } else if (menu_is_app[idx] == 2) {
            wm_open_clock();
        } else if (menu_is_app[idx] == 3) {
            wm_open_window("");
        }
    } else if (active_menu == MENU_FILE) {
        if (menu_is_app[idx] == -3) {
            wm_open_window("");
        } else if (menu_is_app[idx] == -4) {
            if (wm_focused) wm_close_window(wm_focused);
        }
    }
}

static void wm_desktop_tick(void)
{
    mouse_poll();

    int cx, cy;
    if (mouse_get_click(&cx, &cy)) {
        if (active_menu != MENU_NONE) {
            int idx;
            if (dropdown_hit(cx, cy, &idx)) {
                handle_menu_click(idx);
            }
            active_menu = MENU_NONE;
        } else if (cy == MENU_ROW) {
            if (cx >= t_x0 - 1 && cx <= t_x1) active_menu = MENU_T;
            else if (cx >= file_x0 && cx <= file_x1) active_menu = MENU_FILE;
            else if (cx >= edit_x0 && cx <= edit_x1) active_menu = MENU_EDIT;
            else if (cx >= view_x0 && cx <= view_x1) active_menu = MENU_VIEW;
            else if (cx >= label_x0 && cx <= label_x1) active_menu = MENU_LABEL;
            else if (cx >= special_x0 && cx <= special_x1) active_menu = MENU_SPECIAL;
        } else if (cy == DOCK_ROW) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                window_t *w = &windows[i];
                if (w->open && cx >= w->tb_x0 && cx <= w->tb_x1) {
                    if (w == wm_focused && !w->minimized) {
                        w->minimized = 1;
                    } else {
                        wm_focus_window(w);
                    }
                    break;
                }
            }
        } else {
            window_t *hit = NULL;
            int best_z = -1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                window_t *w = &windows[i];
                if (!w->open || w->minimized) continue;
                if (cx >= w->x0 && cx < w->x0 + w->w0 && cy >= w->y0 && cy < w->y0 + w->h0) {
                    if (w->z > best_z) { best_z = w->z; hit = w; }
                }
            }
            if (hit) {
                if (cy == hit->y0) {
                    int gx = hit->x0 + hit->w0 - 4;
                    if (cx == hit->x0 + 1) { wm_close_window(hit); }
                    else if (cx == gx) { hit->minimized = 1; }
                    else if (cx == gx + 1) { hit->maximized = !hit->maximized; }
                    else if (cx == gx + 2) { wm_close_window(hit); }
                    else { wm_focus_window(hit); }
                } else {
                    wm_focus_window(hit);
                }
            }
        }
    }

    uint32_t now = task_get_ticks();
    if (now - last_draw_tick >= FRAME_INTERVAL_TICKS) {
        last_draw_tick = now;

        /* Classic Mac OS desktop: near-white background with a sparse grey
           stipple, instead of a solid teal fill. */
        for (int y = MENU_ROW + 1; y < DOCK_ROW; y++) {
            for (int x = 0; x < VGA_W; x++) {
                if (((x + y * 3) % 7) == 0)
                    vga_put(x, y, 0xFA, mk_color(VGA_LIGHT_GREY, VGA_WHITE));
                else
                    vga_put(x, y, ' ', mk_color(VGA_LIGHT_GREY, VGA_WHITE));
            }
        }

        draw_trash();

        int order[MAX_WINDOWS];
        int n = 0;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (windows[i].open && !windows[i].minimized) order[n++] = i;
        for (int i = 1; i < n; i++) {
            int key = order[i];
            int keyz = windows[key].z;
            int j = i - 1;
            while (j >= 0 && windows[order[j]].z > keyz) { order[j + 1] = order[j]; j--; }
            order[j + 1] = key;
        }
        for (int i = 0; i < n; i++) draw_window(&windows[order[i]]);

        draw_dock();
        draw_menu_bar();

        if (active_menu != MENU_NONE) {
            int ax0, ax1;
            switch (active_menu) {
            case MENU_T:       ax0 = t_x0 - 1;   ax1 = t_x1;     break;
            case MENU_FILE:    ax0 = file_x0;    ax1 = file_x1;  break;
            case MENU_EDIT:    ax0 = edit_x0;    ax1 = edit_x1;  break;
            case MENU_VIEW:    ax0 = view_x0;    ax1 = view_x1;  break;
            case MENU_LABEL:   ax0 = label_x0;   ax1 = label_x1; break;
            default:           ax0 = special_x0; ax1 = special_x1; break;
            }
            draw_dropdown_menu(active_menu, ax0, ax1);
        }

        draw_cursor();

        for (int i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = backbuffer[i];
    }

    task_yield();
}

void wm_poll(void)
{
    if (task_get_userdata()) return;
    wm_desktop_tick();
}

void wm_init(void)
{
    mouse_init();
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].open = 0;
    wm_focused = NULL;
    active_menu = MENU_NONE;
    next_z = 1;
}

void wm_run(void)
{
    for (;;) wm_poll();
}
