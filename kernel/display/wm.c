#include "wm.h"
#include "terminal.h"
#include "mouse.h"
#include "scheduler.h"
#include "shell.h"
#include "string.h"
#include "cmos.h"
#include "net.h"
#include "notepad.h"

#define VGA_W 80
#define VGA_H 25
#define TASKBAR_ROW (VGA_H - 1)

#define MAX_WINDOWS 6
#define NORMAL_W 60
#define NORMAL_H 16

#define WIN_KIND_TERMINAL 0
#define WIN_KIND_NOTEPAD 1

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
static int start_menu_open = 0;
static int start_btn_x0, start_btn_x1;

#define MENU_MAX_ITEMS 64
static int menu_x0[MENU_MAX_ITEMS];
static int menu_y0[MENU_MAX_ITEMS];
static int menu_x1[MENU_MAX_ITEMS];
static int menu_count = 0;
static const char *menu_names[MENU_MAX_ITEMS];
static int menu_is_app[MENU_MAX_ITEMS];

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
        *x0 = 0; *y0 = 0; *w0 = VGA_W; *h0 = VGA_H - 1;
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

static void draw_window(window_t *w)
{
    int x0, y0, w0, h0;
    window_geom(w, &x0, &y0, &w0, &h0);

    uint8_t title_color = (w == wm_focused) ? mk_color(VGA_WHITE, VGA_BLUE) : mk_color(VGA_LIGHT_GREY, VGA_DARK_GREY);

    vga_fill_rect(x0, y0, w0, 1, ' ', title_color);
    vga_text(x0 + 1, y0, w->title, title_color);

    int gx = x0 + w0 - 4;
    vga_put(gx, y0, '_', title_color);
    vga_put(gx + 1, y0, '#', title_color);
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

static int start_menu_hit(int cx, int cy, int *out_index)
{
    for (int i = 0; i < menu_count; i++) {
        if (cx >= menu_x0[i] && cx <= menu_x1[i] && cy == menu_y0[i]) { *out_index = i; return 1; }
    }
    return 0;
}

static void draw_start_menu(void)
{
    const char **builtin = shell_builtin_names();
    int count = 0;
    while (builtin[count] && count < MENU_MAX_ITEMS - 1) {
        menu_names[count] = builtin[count];
        menu_is_app[count] = 0;
        count++;
    }
    menu_names[count] = "Notepad";
    menu_is_app[count] = 1;
    count++;
    menu_count = count;

    int cols = 3;
    int rows = (count + cols - 1) / cols;
    int item_w = 14;
    int menu_w = cols * item_w + 2;
    int menu_h = rows + 2;
    int mx0 = 0;
    int my0 = TASKBAR_ROW - menu_h;
    if (my0 < 0) my0 = 0;

    vga_fill_rect(mx0, my0, menu_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_LIGHT_GREY));
    vga_text(mx0 + 1, my0, "Programs", mk_color(VGA_BLACK, VGA_LIGHT_GREY));

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;
        int ix = mx0 + 1 + col * item_w;
        int iy = my0 + 1 + row;
        if (iy >= my0 + menu_h - 1) continue;
        vga_text(ix, iy, menu_names[i], mk_color(VGA_BLACK, VGA_LIGHT_GREY));
        menu_x0[i] = ix; menu_y0[i] = iy; menu_x1[i] = ix + item_w - 1;
    }
}

static void draw_taskbar(void)
{
    vga_fill_rect(0, TASKBAR_ROW, VGA_W, 1, ' ', mk_color(VGA_LIGHT_GREY, VGA_DARK_GREY));

    uint8_t start_color = start_menu_open ? mk_color(VGA_BLACK, VGA_LIGHT_GREY) : mk_color(VGA_WHITE, VGA_GREEN);
    vga_text(0, TASKBAR_ROW, " Start ", start_color);
    start_btn_x0 = 0; start_btn_x1 = 6;

    int x = 8;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open) continue;
        char buf[12];
        int k = 0;
        while (w->title[k] && k < 10) { buf[k] = w->title[k]; k++; }
        buf[k] = 0;
        int len = (int)strlen(buf) + 2;
        if (x + len >= VGA_W - 16) break;
        uint8_t c = (w == wm_focused && !w->minimized) ? mk_color(VGA_BLACK, VGA_WHITE) : mk_color(VGA_WHITE, VGA_DARK_GREY);
        vga_put(x, TASKBAR_ROW, '[', c);
        vga_text(x + 1, TASKBAR_ROW, buf, c);
        vga_put(x + 1 + (int)strlen(buf), TASKBAR_ROW, ']', c);
        w->tb_x0 = x; w->tb_x1 = x + len - 1;
        x += len + 1;
    }

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
    right[rk++] = ' '; right[rk++] = ' ';
    rk += fmt_uint02(right + rk, (unsigned int)t.hour);
    right[rk++] = ':';
    rk += fmt_uint02(right + rk, (unsigned int)t.minute);
    right[rk] = 0;

    int rlen = (int)strlen(right);
    vga_text(VGA_W - rlen - 1, TASKBAR_ROW, right, mk_color(VGA_WHITE, VGA_DARK_GREY));
}

static void draw_cursor(void)
{
    int mx, my; uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    (void)btns;
    if (mx < 0 || mx >= VGA_W || my < 0 || my >= VGA_H) return;
    uint16_t cell = backbuffer[my * VGA_W + mx];
    char ch = (char)(cell & 0xFF);
    uint8_t fg = (cell >> 8) & 0x0F;
    uint8_t bg = (cell >> 12) & 0x0F;
    backbuffer[my * VGA_W + mx] = mk_cell(ch, mk_color(bg, fg));
}

#define FRAME_INTERVAL_TICKS 1
static uint32_t last_draw_tick = 0;

static void wm_desktop_tick(void)
{
    mouse_poll();

    int cx, cy;
    if (mouse_get_click(&cx, &cy)) {
        if (start_menu_open) {
            int idx;
            if (start_menu_hit(cx, cy, &idx)) {
                start_menu_open = 0;
                if (menu_is_app[idx])
                    wm_open_notepad();
                else
                    wm_open_window(menu_names[idx]);
            } else {
                start_menu_open = 0;
            }
        } else if (cy == TASKBAR_ROW) {
            if (cx >= start_btn_x0 && cx <= start_btn_x1) {
                start_menu_open = 1;
            } else {
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
                    if (cx == gx) { hit->minimized = 1; }
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

        vga_fill_rect(0, 0, VGA_W, VGA_H - 1, ' ', mk_color(VGA_LIGHT_GREY, VGA_CYAN));

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

        if (start_menu_open) draw_start_menu();
        draw_taskbar();
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
    start_menu_open = 0;
    next_z = 1;
}

void wm_run(void)
{
    for (;;) wm_poll();
}
