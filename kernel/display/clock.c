#include "clock.h"
#include "terminal.h"
#include "keyboard.h"
#include "cmos.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"

#define CLK_COLS 60
#define CLK_ROWS 15

#define VIEW_DIGITAL 0
#define VIEW_ANALOG 1
#define VIEW_STOPWATCH 2

static int sw_running = 0;
static uint32_t sw_start_tick = 0;
static uint32_t sw_accum_ticks = 0;

static uint8_t clk_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void put_char_at(int x, int y, char c, uint8_t color)
{
    if (x < 0 || x >= CLK_COLS || y < 0 || y >= CLK_ROWS) return;
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar(c);
}

/* Full-area blank, used only on window open and on view switch (not every
 * redraw tick) so static views don't flicker — see about.c for the same
 * fix applied to the About dialog. The Analog view still calls this every
 * tick since its hands genuinely move and must be erased each frame. */
static void clear_area(void)
{
    uint8_t bg = clk_color(VGA_BLACK, VGA_LIGHT_GREY);
    for (int r = 0; r < CLK_ROWS; r++) put_str(0, r, "                                                            ", bg);
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

static int fmt2(char *buf, int v)
{
    if (v < 0) v = 0;
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + v % 10;
    return 2;
}

static void draw_header(int view)
{
    uint8_t base = clk_color(VGA_BLACK, VGA_LIGHT_GREY);
    uint8_t hi = clk_color(VGA_WHITE, VGA_BLUE);
    const char *labels[3] = { " Digital ", " Analog ", " Stopwatch " };
    int x = 1;
    for (int i = 0; i < 3; i++) {
        uint8_t c = (i == view) ? hi : base;
        put_str(x, 0, labels[i], c);
        x += (int)strlen(labels[i]) + 1;
    }
}

static void draw_digital(void)
{
    cmos_time_t t;
    cmos_get_time(&t);

    char buf[9];
    int k = 0;
    k += fmt2(buf + k, t.hour); buf[k++] = ':';
    k += fmt2(buf + k, t.minute); buf[k++] = ':';
    k += fmt2(buf + k, t.second); buf[k] = 0;

    int x = (CLK_COLS - (int)strlen(buf)) / 2;
    put_str(x, 6, buf, clk_color(VGA_BLACK, VGA_LIGHT_GREY));

    char dbuf[16];
    int dk = 0;
    dk += fmt2(dbuf + dk, t.day); dbuf[dk++] = '/';
    dk += fmt2(dbuf + dk, t.month); dbuf[dk++] = '/';
    dk += fmt_uint(dbuf + dk, (unsigned int)t.year); dbuf[dk] = 0;

    int dx = (CLK_COLS - (int)strlen(dbuf)) / 2;
    put_str(dx, 8, dbuf, clk_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
}

static const int8_t dir8[8][2] = {
    { 0, -1}, { 1, -1}, { 1, 0}, { 1, 1},
    { 0,  1}, {-1,  1}, {-1, 0}, {-1, -1},
};

static int octant_for(int pos60)
{
    return ((pos60 * 8) + 30) / 60 % 8;
}

static void draw_hand(int cx, int cy, int octant, int len, int xscale, char ch, uint8_t color)
{
    int dx = dir8[octant][0];
    int dy = dir8[octant][1];
    for (int i = 1; i <= len; i++)
        put_char_at(cx + dx * i * xscale, cy + dy * i, ch, color);
}

static void draw_analog(void)
{
    cmos_time_t t;
    cmos_get_time(&t);

    int cx = CLK_COLS / 2;
    int cy = 1 + (CLK_ROWS - 1) / 2;
    int rx = 14, ry = 6;

    uint8_t face = clk_color(VGA_BLACK, VGA_LIGHT_GREY);

    put_str(cx - 1, cy - ry, "12", face);
    put_str(cx + rx, cy, "3", face);
    put_str(cx, cy + ry, "6", face);
    put_str(cx - rx, cy, "9", face);

    int drx = rx * 7 / 10, dry = ry * 7 / 10;
    put_char_at(cx + drx, cy - dry, '.', face);
    put_char_at(cx + drx, cy + dry, '.', face);
    put_char_at(cx - drx, cy + dry, '.', face);
    put_char_at(cx - drx, cy - dry, '.', face);

    int hour_pos = (t.hour % 12) * 5 + t.minute / 12;

    draw_hand(cx, cy, octant_for(t.second), 6, 2, '.', clk_color(VGA_RED, VGA_LIGHT_GREY));
    draw_hand(cx, cy, octant_for(t.minute), 5, 2, '+', clk_color(VGA_BLACK, VGA_LIGHT_GREY));
    draw_hand(cx, cy, octant_for(hour_pos), 3, 2, '#', clk_color(VGA_BLACK, VGA_LIGHT_GREY));

    put_char_at(cx, cy, 'o', clk_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void draw_stopwatch(void)
{
    uint32_t elapsed = sw_accum_ticks;
    if (sw_running) elapsed += task_get_ticks() - sw_start_tick;

    uint32_t total_cs = elapsed;
    uint32_t minutes = (total_cs / 100) / 60;
    uint32_t seconds = (total_cs / 100) % 60;
    uint32_t centis = total_cs % 100;

    char buf[16];
    int k = 0;
    k += fmt2(buf + k, (int)minutes); buf[k++] = ':';
    k += fmt2(buf + k, (int)seconds); buf[k++] = '.';
    k += fmt2(buf + k, (int)centis); buf[k] = 0;

    int x = (CLK_COLS - (int)strlen(buf)) / 2;
    put_str(x, 6, buf, clk_color(VGA_BLACK, VGA_LIGHT_GREY));

    const char *status = sw_running ? "RUNNING" : "STOPPED";
    int sx = (CLK_COLS - (int)strlen(status)) / 2;
    put_str(sx, 8, status, sw_running ? clk_color(VGA_WHITE, VGA_GREEN) : clk_color(VGA_WHITE, VGA_RED));

    const char *hint = "[Space] Start/Stop   [R] Reset";
    int hx = (CLK_COLS - (int)strlen(hint)) / 2;
    put_str(hx, 11, hint, clk_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
}

void clock_run(void)
{
    int view = VIEW_DIGITAL;
    sw_running = 0;
    sw_start_tick = 0;
    sw_accum_ticks = 0;

    terminal_clear();
    clear_area();

    uint32_t last_redraw = 0;
    int prev_view = view;

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == '1') view = VIEW_DIGITAL;
            else if (c == '2') view = VIEW_ANALOG;
            else if (c == '3') view = VIEW_STOPWATCH;
            else if (view == VIEW_STOPWATCH) {
                if (c == ' ') {
                    if (sw_running) {
                        sw_accum_ticks += task_get_ticks() - sw_start_tick;
                        sw_running = 0;
                    } else {
                        sw_start_tick = task_get_ticks();
                        sw_running = 1;
                    }
                } else if (c == 'r' || c == 'R') {
                    sw_running = 0;
                    sw_accum_ticks = 0;
                }
            }
        }

        uint32_t now = task_get_ticks();
        if (now - last_redraw >= 5) {
            last_redraw = now;
            if (view != prev_view) { clear_area(); prev_view = view; }
            else if (view == VIEW_ANALOG) clear_area();
            draw_header(view);
            if (view == VIEW_DIGITAL) draw_digital();
            else if (view == VIEW_ANALOG) draw_analog();
            else draw_stopwatch();
        }

        task_yield();
    }
}
