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
#include "about.h"
#include "diskmgr.h"
#include "calculator.h"
#include "filemgr.h"
#include "paint.h"
#include "viewer.h"
#include "taskmgr.h"

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
#define WIN_KIND_ABOUT 3
#define WIN_KIND_DISKMGR 4
#define WIN_KIND_CALCULATOR 5
#define WIN_KIND_FILEMGR 6
#define WIN_KIND_PAINT 7
#define WIN_KIND_VIEWER 8
#define WIN_KIND_TASKMGR 9

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
    int rest_x0, rest_y0, rest_w0, rest_h0;
    int tb_x0, tb_x1;
} window_t;

static window_t *drag_window;
static int drag_offset_x, drag_offset_y;

static window_t windows[MAX_WINDOWS];
static window_t *wm_focused = NULL;
static window_t *pending_window;
static int next_z = 1;

/* One-shot mouse click within a window's content area (below the titlebar),
 * in surface-local coordinates, claimable by that window's own task via
 * wm_get_content_click(). Cleared on read or overwritten by the next click. */
static window_t *content_click_target = NULL;
static int content_click_x, content_click_y;

/* One-shot menu action requested for a window via the top File/Edit/... bar,
 * claimable by that window's own task via wm_get_menu_action(). */
static window_t *action_target = NULL;
static int pending_action = WM_ACTION_NONE;

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

int wm_get_content_click(int *x, int *y)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || content_click_target != self) return 0;
    content_click_target = NULL;
    if (x) *x = content_click_x;
    if (y) *y = content_click_y;
    return 1;
}

/* Continuous (per-frame) content-relative mouse position + live button
 * mask for the focused window's own task, used for drag gestures (e.g.
 * Paint) where a one-shot click isn't enough. */
int wm_get_content_mouse(int *x, int *y, int *buttons)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || wm_focused != self) return 0;
    int mx, my;
    uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    if (x) *x = mx - self->x0;
    if (y) *y = my - self->y0 - 1;
    if (buttons) *buttons = btns;
    return 1;
}

int wm_get_menu_action(void)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || action_target != self || pending_action == WM_ACTION_NONE) return WM_ACTION_NONE;
    int a = pending_action;
    action_target = NULL;
    pending_action = WM_ACTION_NONE;
    return a;
}

/* Computes the initial position/size for a newly opened window (called
 * once at creation, not on every redraw) and stores both the active
 * geometry and the "restore" geometry to return to when un-maximized.
 * Windows are freely draggable afterwards by their title bar; this only
 * ever runs again when a window is opened. */
static void window_geom_init(window_t *w)
{
    int slot = (int)(w - windows);
    w->rest_x0 = 4 + (slot % 4) * 3;
    w->rest_y0 = 2 + (slot % 3) * 2;
    w->rest_w0 = NORMAL_W;
    w->rest_h0 = NORMAL_H;
    if (w->maximized) {
        w->x0 = 0; w->y0 = MENU_ROW + 1; w->w0 = VGA_W; w->h0 = VGA_H - 2;
    } else {
        w->x0 = w->rest_x0; w->y0 = w->rest_y0; w->w0 = w->rest_w0; w->h0 = w->rest_h0;
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
    else if (w->kind == WIN_KIND_ABOUT)
        about_run();
    else if (w->kind == WIN_KIND_DISKMGR)
        diskmgr_run();
    else if (w->kind == WIN_KIND_CALCULATOR)
        calculator_run();
    else if (w->kind == WIN_KIND_FILEMGR)
        filemgr_run();
    else if (w->kind == WIN_KIND_PAINT)
        paint_run();
    else if (w->kind == WIN_KIND_VIEWER)
        viewer_run();
    else if (w->kind == WIN_KIND_TASKMGR)
        taskmgr_run();
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

/* Killing a window's task out from under it (e.g. from Task Manager)
 * needs the same cleanup wm_close_window() does — open/minimized/
 * maximized flags and wm_focused — not just task_kill(), or the now-dead
 * window keeps drawing its last frame forever. Returns 0 if pid matched
 * an open window (and it was closed), -1 if pid isn't a window task. */
int wm_kill_task_window(uint32_t pid)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].open && (uint32_t)windows[i].pid == pid) {
            wm_close_window(&windows[i]);
            return 0;
        }
    }
    return -1;
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

    window_geom_init(w);

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

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

void wm_open_notepad_file(const char *path)
{
    notepad_open_path(path);
    wm_open_notepad();
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

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_about(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_ABOUT;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "About This Computer");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_diskmgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_DISKMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Disk Utility");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_calculator(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_CALCULATOR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Calculator");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_filemgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_FILEMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Files");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_paint(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_PAINT;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Paint");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_viewer(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_VIEWER;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Image Viewer");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

void wm_open_viewer_file(const char *path)
{
    viewer_open_path(path);
    wm_open_viewer();
}

static void wm_open_taskmgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_TASKMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Task Manager");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void draw_window(window_t *w)
{
    int x0 = w->x0, y0 = w->y0, w0 = w->w0, h0 = w->h0;

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
    int vo = w->surface.view_offset;
    for (int row = 0; row < content_h; row++) {
        for (int col = 0; col < w0; col++) {
            uint16_t cell;
            int shown = row - vo;
            if (shown >= 0 && shown < TERM_SURFACE_H && col < TERM_SURFACE_W) {
                cell = w->surface.cells[shown * TERM_SURFACE_W + col];
            } else if (shown < 0 && col < TERM_SURFACE_W) {
                const uint16_t *sb = terminal_scrollback_row(&w->surface, -shown);
                cell = sb ? sb[col] : mk_cell(' ', mk_color(VGA_LIGHT_GREY, VGA_BLACK));
            } else {
                cell = mk_cell(' ', mk_color(VGA_LIGHT_GREY, VGA_BLACK));
            }
            int vx = x0 + col, vy = y0 + 1 + row;
            if (vx >= 0 && vx < VGA_W && vy >= 0 && vy < VGA_H - 1)
                backbuffer[vy * VGA_W + vx] = cell;
        }
    }

    /* Scrollback slider in the rightmost content column, terminal windows
     * only — other app kinds (e.g. Notepad) manage that column themselves. */
    if (w->kind == WIN_KIND_TERMINAL && w->surface.scrollback_count > 0) {
        int max_off = w->surface.scrollback_count;
        int thumb_row = content_h - 1 - (content_h > 1 ? ((content_h - 1) * vo) / max_off : 0);
        for (int row = 0; row < content_h; row++) {
            int vx = x0 + w0 - 1, vy = y0 + 1 + row;
            if (vx < 0 || vx >= VGA_W || vy < 0 || vy >= VGA_H - 1) continue;
            if (row == thumb_row)
                backbuffer[vy * VGA_W + vx] = mk_cell(0xDB, mk_color(VGA_WHITE, VGA_BLUE));
            else
                backbuffer[vy * VGA_W + vx] = mk_cell(0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
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
        menu_names[menu_count] = "About This Computer..."; menu_is_app[menu_count] = 4; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Notepad"; menu_is_app[menu_count] = 1; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Files"; menu_is_app[menu_count] = 7; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Paint"; menu_is_app[menu_count] = 8; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Image Viewer"; menu_is_app[menu_count] = 9; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Task Manager"; menu_is_app[menu_count] = 10; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Calculator"; menu_is_app[menu_count] = 6; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Terminal"; menu_is_app[menu_count] = 3; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Clock"; menu_is_app[menu_count] = 2; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Disk Utility"; menu_is_app[menu_count] = 5; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Shut Down"; menu_is_app[menu_count] = -2; menu_disabled[menu_count] = 0;
        menu_count++;
    } else if (which == MENU_FILE) {
        if (wm_focused && wm_focused->kind == WIN_KIND_NOTEPAD) {
            menu_names[menu_count] = "New"; menu_is_app[menu_count] = -5; menu_disabled[menu_count] = 0; menu_count++;
            menu_names[menu_count] = "Open..."; menu_is_app[menu_count] = -6; menu_disabled[menu_count] = 0; menu_count++;
            menu_names[menu_count] = "Save"; menu_is_app[menu_count] = -7; menu_disabled[menu_count] = 0; menu_count++;
        }
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
    int item_w = 0;
    for (int i = 0; i < apps_count; i++) {
        int len = (int)strlen(menu_names[apps_start + i]) + 2;
        if (len > item_w) item_w = len;
    }
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

    uint8_t header_color = menu_disabled[header_idx] ? mk_color(VGA_LIGHT_GREY, VGA_WHITE) : mk_color(VGA_BLACK, VGA_WHITE);
    vga_text(mx0 + 1, my0, menu_names[header_idx], header_color);
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

    t_x0 = 1; t_x1 = 2;
    vga_put(t_x0, MENU_ROW, 'T', mk_color(VGA_RED, VGA_WHITE));

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

static const char *get_icon_char(int kind)
{
    switch (kind) {
        case WIN_KIND_TERMINAL: return ">";
        case WIN_KIND_NOTEPAD:  return "\xF8";
        case WIN_KIND_CLOCK:    return "\xF8";
        case WIN_KIND_ABOUT:    return "?";
        case WIN_KIND_DISKMGR:  return "\xF7";
        case WIN_KIND_CALCULATOR: return "#";
        case WIN_KIND_FILEMGR:  return "\xF6";
        case WIN_KIND_PAINT:    return "\x0E";
        case WIN_KIND_VIEWER:   return "\x0C";
        case WIN_KIND_TASKMGR:  return "\x05";
        default:                return ".";
    }
}

static void draw_dock(void)
{
    vga_fill_rect(0, DOCK_ROW, VGA_W, 1, ' ', mk_color(VGA_BLACK, VGA_LIGHT_GREY));

    int x = 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open) continue;
        char buf[14];
        int k = 0;
        buf[k++] = ' ';
        const char *icon = get_icon_char(w->kind);
        while (*icon) buf[k++] = *icon++;
        buf[k++] = ' ';
        int j = 0;
        while (w->title[j] && k < 12) { buf[k++] = w->title[j++]; }
        buf[k] = 0;
        int len = k + 2;
        if (x + len >= VGA_W - 8) break;
        uint8_t c = (w == wm_focused && !w->minimized) ? mk_color(VGA_WHITE, VGA_BLUE) : mk_color(VGA_BLACK, VGA_LIGHT_GREY);
        uint8_t icon_c = (w == wm_focused && !w->minimized) ? mk_color(VGA_LIGHT_CYAN, VGA_BLUE) : mk_color(VGA_DARK_GREY, VGA_LIGHT_GREY);
        vga_put(x, DOCK_ROW, '[', c);
        vga_put(x + 1, DOCK_ROW, buf[0], icon_c);
        vga_put(x + 2, DOCK_ROW, buf[1], icon_c);
        vga_put(x + 3, DOCK_ROW, ' ', c);
        vga_text(x + 4, DOCK_ROW, buf + 3, c);
        vga_put(x + len - 1, DOCK_ROW, ']', c);
        w->tb_x0 = x; w->tb_x1 = x + len - 1;
        x += len + 1;
    }
}

static void draw_trash(void)
{
    int x = VGA_W - 7;
    int y = DOCK_ROW - 6;
    uint8_t line = mk_color(VGA_BLACK, VGA_WHITE);

    vga_put(x + 2, y, 0xD5, line);
    vga_put(x + 3, y, 0xC4, line);
    vga_put(x + 4, y, 0xB8, line);

    vga_put(x + 1, y + 1, 0xC4, line);
    vga_put(x + 2, y + 1, 0xD1, line);
    vga_put(x + 3, y + 1, 0xC4, line);
    vga_put(x + 4, y + 1, 0xD1, line);
    vga_put(x + 5, y + 1, 0xC4, line);

    vga_put(x + 1, y + 2, 0xB3, line);
    vga_put(x + 2, y + 2, ' ', line);
    vga_put(x + 3, y + 2, 0xB3, line);
    vga_put(x + 4, y + 2, ' ', line);
    vga_put(x + 5, y + 2, 0xB3, line);

    vga_put(x + 1, y + 3, 0xB3, line);
    vga_put(x + 2, y + 3, ' ', line);
    vga_put(x + 3, y + 3, 0xB3, line);
    vga_put(x + 4, y + 3, ' ', line);
    vga_put(x + 5, y + 3, 0xB3, line);

    vga_put(x, y + 4, 0xC0, line);
    vga_fill_rect(x + 1, y + 4, 5, 1, 0xC4, line);
    vga_put(x + 6, y + 4, 0xD9, line);

    vga_text(x, y + 5, " Trash ", mk_color(VGA_BLACK, VGA_WHITE));
}

static void draw_cursor(void)
{
    int mx, my; uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    (void)btns;
    if (mx < 0 || mx >= VGA_W || my < 0 || my >= VGA_H) return;
    uint16_t cell = backbuffer[my * VGA_W + mx];
    uint8_t bg = (cell >> 12) & 0x0F;
    uint8_t cursor_fg = (bg == VGA_BLACK) ? VGA_WHITE : VGA_BLACK;
    backbuffer[my * VGA_W + mx] = mk_cell(0x10, mk_color(cursor_fg, bg));
}

#define FRAME_INTERVAL_TICKS 1
static uint32_t last_draw_tick = 0;

static void wm_shutdown(void)
{
    uint8_t color = mk_color(VGA_WHITE, VGA_BLACK);
    for (int y = 0; y < VGA_H; y++)
        for (int x = 0; x < VGA_W; x++)
            backbuffer[y * VGA_W + x] = mk_cell(' ', color);

    const char *lines[] = {
        "tOS",
        "",
        "It is now safe to turn off your computer.",
    };
    int start_y = VGA_H / 2 - 1;
    for (int i = 0; i < 3; i++) {
        const char *s = lines[i];
        int len = (int)strlen(s);
        int x = (VGA_W - len) / 2;
        vga_text(x, start_y + i, s, color);
    }

    for (int i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = backbuffer[i];

    for (;;) asm volatile("hlt");
}

static void handle_menu_click(int idx)
{
    if (active_menu == MENU_T) {
        if (menu_is_app[idx] == -2) {
            wm_shutdown();
        } else if (menu_is_app[idx] == 1) {
            wm_open_notepad();
        } else if (menu_is_app[idx] == 2) {
            wm_open_clock();
        } else if (menu_is_app[idx] == 3) {
            wm_open_window("");
        } else if (menu_is_app[idx] == 4) {
            wm_open_about();
        } else if (menu_is_app[idx] == 5) {
            wm_open_diskmgr();
        } else if (menu_is_app[idx] == 6) {
            wm_open_calculator();
        } else if (menu_is_app[idx] == 7) {
            wm_open_filemgr();
        } else if (menu_is_app[idx] == 8) {
            wm_open_paint();
        } else if (menu_is_app[idx] == 9) {
            wm_open_viewer();
        } else if (menu_is_app[idx] == 10) {
            wm_open_taskmgr();
        }
    } else if (active_menu == MENU_FILE) {
        if (menu_is_app[idx] == -3) {
            wm_open_window("");
        } else if (menu_is_app[idx] == -4) {
            if (wm_focused) wm_close_window(wm_focused);
        } else if (menu_is_app[idx] == -5) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_NEW; }
        } else if (menu_is_app[idx] == -6) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_OPEN; }
        } else if (menu_is_app[idx] == -7) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_SAVE; }
        }
    }
}

static void wm_desktop_tick(void)
{
    mouse_poll();

    if (drag_window) {
        int mx, my;
        uint8_t btns;
        mouse_get_state(&mx, &my, &btns);
        if (btns & 1) {
            int nx = mx - drag_offset_x;
            int ny = my - drag_offset_y;
            if (nx < 0) nx = 0;
            if (ny < MENU_ROW + 1) ny = MENU_ROW + 1;
            if (nx > VGA_W - drag_window->w0) nx = VGA_W - drag_window->w0;
            if (ny > DOCK_ROW - 1) ny = DOCK_ROW - 1;
            drag_window->x0 = nx;
            drag_window->y0 = ny;
        } else {
            drag_window = NULL;
        }
    }

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
                    else if (cx == gx + 1) {
                        if (!hit->maximized) {
                            hit->rest_x0 = hit->x0; hit->rest_y0 = hit->y0;
                            hit->rest_w0 = hit->w0; hit->rest_h0 = hit->h0;
                            hit->maximized = 1;
                            hit->x0 = 0; hit->y0 = MENU_ROW + 1; hit->w0 = VGA_W; hit->h0 = VGA_H - 2;
                        } else {
                            hit->maximized = 0;
                            hit->x0 = hit->rest_x0; hit->y0 = hit->rest_y0;
                            hit->w0 = hit->rest_w0; hit->h0 = hit->rest_h0;
                        }
                    }
                    else if (cx == gx + 2) { wm_close_window(hit); }
                    else {
                        wm_focus_window(hit);
                        if (!hit->maximized) {
                            drag_window = hit;
                            drag_offset_x = cx - hit->x0;
                            drag_offset_y = cy - hit->y0;
                        }
                    }
                } else if (hit->kind == WIN_KIND_TERMINAL && hit->surface.scrollback_count > 0 &&
                           cx == hit->x0 + hit->w0 - 1) {
                    wm_focus_window(hit);
                    int content_h = hit->h0 - 1;
                    int row_clicked = cy - hit->y0 - 1;
                    int max_off = hit->surface.scrollback_count;
                    int vo = (content_h > 1)
                        ? max_off - (row_clicked * max_off) / (content_h - 1)
                        : 0;
                    if (vo < 0) vo = 0;
                    if (vo > max_off) vo = max_off;
                    hit->surface.view_offset = vo;
                } else {
                    int was_focused = (hit == wm_focused);
                    wm_focus_window(hit);
                    if (was_focused) {
                        content_click_target = hit;
                        content_click_x = cx - hit->x0;
                        content_click_y = cy - hit->y0 - 1;
                    }
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
