#include "viewer.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"
#include "png.h"

#define VIEWER_COLS 79
#define VIEWER_ROWS 22
#define TOOLBAR_ROW 0
#define CANVAS_Y0 1
#define STATUS_ROW (VIEWER_ROWS - 1)
#define CANVAS_H (STATUS_ROW - CANVAS_Y0)
#define CANVAS_W VIEWER_COLS

#define NUM_BUTTONS 4
static const char *btn_labels[NUM_BUTTONS] = { "Open", "Zoom In", "Zoom Out", "Fit" };
static int btn_x0[NUM_BUTTONS], btn_x1[NUM_BUTTONS];

static uint8_t *img_rgb;
static uint32_t img_w, img_h;
static int scale;
static int view_x, view_y;
static char filename[64];
static char status_msg[VIEWER_COLS + 1];
static char pending_path[64];
static int has_pending;

static const uint8_t vga_palette[16][3] = {
    {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
    {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
    {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

void viewer_open_path(const char *path)
{
    strncpy(pending_path, path, sizeof(pending_path) - 1);
    pending_path[sizeof(pending_path) - 1] = 0;
    has_pending = 1;
}

static uint8_t pcolor(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < VIEWER_COLS) { status_msg[i] = s[i]; i++; }
    status_msg[i] = 0;
}

static int fmt_uint(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int nearest_vga_color(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0;
    int best_dist = -1;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga_palette[i][0];
        int dg = g - vga_palette[i][1];
        int db = b - vga_palette[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) { best_dist = dist; best = i; }
    }
    return best;
}

static void free_image(void)
{
    if (img_rgb) { free(img_rgb); img_rgb = NULL; }
    img_w = img_h = 0;
}

static void clamp_view(void)
{
    if (scale < 1) scale = 1;
    int max_vx = (int)img_w - CANVAS_W * scale;
    int max_vy = (int)img_h - CANVAS_H * scale;
    if (max_vx < 0) max_vx = 0;
    if (max_vy < 0) max_vy = 0;
    if (view_x > max_vx) view_x = max_vx;
    if (view_y > max_vy) view_y = max_vy;
    if (view_x < 0) view_x = 0;
    if (view_y < 0) view_y = 0;
}

static void fit_view(void)
{
    int sx = ((int)img_w + CANVAS_W - 1) / CANVAS_W;
    int sy = ((int)img_h + CANVAS_H - 1) / CANVAS_H;
    scale = sx > sy ? sx : sy;
    if (scale < 1) scale = 1;
    view_x = 0;
    view_y = 0;
}

static void do_open(const char *path)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) {
        set_status("Open: file not found.");
        return;
    }
    uint32_t sz = fsbridge_size(path);
    uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
    if (!buf) { set_status("Open: out of memory."); return; }
    fsbridge_read(path, buf, sz, 0);

    uint8_t *new_rgb = NULL;
    uint32_t nw = 0, nh = 0;
    char err[80];
    int rc = png_decode(buf, sz, &new_rgb, &nw, &nh, err, sizeof(err));
    free(buf);

    if (rc != 0) {
        set_status(err);
        return;
    }

    free_image();
    img_rgb = new_rgb;
    img_w = nw;
    img_h = nh;
    strncpy(filename, path, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;
    fit_view();
    set_status("Loaded.");
}

static void prompt_filename(const char *prompt, char *buf, int max)
{
    put_str(0, STATUS_ROW, "                                                                               ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_ROW, prompt, pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)strlen(prompt), (size_t)STATUS_ROW);
    keyboard_readline(buf, max);
}

static void draw_toolbar(void)
{
    put_str(0, TOOLBAR_ROW, "                                                                               ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    int x = 1;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        const char *lbl = btn_labels[i];
        int len = (int)strlen(lbl);
        int w = len + 4;
        btn_x0[i] = x;
        btn_x1[i] = x + w - 1;

        terminal_setpos((size_t)x, TOOLBAR_ROW);
        terminal_setcolor(pcolor(VGA_WHITE, VGA_BLUE));
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 1;
    }
}

static void draw_canvas(void)
{
    for (int cy = 0; cy < CANVAS_H; cy++) {
        terminal_setpos(0, (size_t)(CANVAS_Y0 + cy));
        for (int cx = 0; cx < CANVAS_W; cx++) {
            uint8_t color;
            if (!img_rgb) {
                color = pcolor(VGA_DARK_GREY, VGA_BLACK);
            } else {
                uint32_t sx = (uint32_t)(view_x + cx * scale);
                uint32_t sy = (uint32_t)(view_y + cy * scale);
                if (sx >= img_w || sy >= img_h) {
                    color = pcolor(VGA_DARK_GREY, VGA_BLACK);
                } else {
                    const uint8_t *px = img_rgb + ((uint64_t)sy * img_w + sx) * 3;
                    int vc = nearest_vga_color(px[0], px[1], px[2]);
                    color = pcolor((uint8_t)vc, (uint8_t)vc);
                }
            }
            terminal_setcolor(color);
            terminal_putchar(' ');
        }
    }
}

static void draw_status(void)
{
    char line[VIEWER_COLS + 1];
    int k = 0;
    if (img_rgb) {
        const char *name = filename[0] ? filename : "Untitled";
        while (name[k] && k < 30) { line[k] = name[k]; k++; }
        line[k++] = ' '; line[k++] = '(';
        k += fmt_uint(line + k, img_w);
        line[k++] = 'x';
        k += fmt_uint(line + k, img_h);
        line[k++] = ')'; line[k++] = ' '; line[k++] = ' ';
        line[k++] = 'Z'; line[k++] = 'o'; line[k++] = 'o'; line[k++] = 'm'; line[k++] = ':'; line[k++] = ' ';
        line[k++] = '1'; line[k++] = ':';
        k += fmt_uint(line + k, (uint32_t)scale);
    } else {
        const char *s = status_msg[0] ? status_msg : "No image open. Click Open to load a .png file.";
        while (s[k] && k < VIEWER_COLS) { line[k] = s[k]; k++; }
    }
    while (k < VIEWER_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_ROW, line, pcolor(VGA_BLACK, VGA_LIGHT_GREY));
}

static void redraw(void)
{
    draw_toolbar();
    draw_canvas();
    draw_status();
}

static void handle_toolbar_click(int ccx)
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (ccx >= btn_x0[i] && ccx <= btn_x1[i]) {
            if (i == 0) {
                char path[64];
                prompt_filename("Open: ", path, sizeof(path));
                if (path[0]) do_open(path);
            } else if (i == 1) {
                if (img_rgb) { scale = scale > 1 ? scale - 1 : 1; clamp_view(); }
            } else if (i == 2) {
                if (img_rgb) { scale += 1; clamp_view(); }
            } else if (i == 3) {
                if (img_rgb) fit_view();
            }
            return;
        }
    }
}

void viewer_run(void)
{
    free_image();
    scale = 1;
    view_x = view_y = 0;
    filename[0] = 0;
    status_msg[0] = 0;

    if (has_pending) {
        do_open(pending_path);
        has_pending = 0;
    }

    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == TOOLBAR_ROW) handle_toolbar_click(ccx);
            redraw();
            task_yield();
            continue;
        }

        int spec = keyboard_get_special();
        if (spec && img_rgb) {
            int step = scale * 2;
            if (spec == 1) view_x -= step;
            else if (spec == 2) view_x += step;
            else if (spec == 3) view_y -= step;
            else if (spec == 4) view_y += step;
            clamp_view();
            redraw();
            task_yield();
            continue;
        }

        task_yield();
    }
}
