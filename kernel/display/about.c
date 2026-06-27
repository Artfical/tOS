#include "about.h"
#include "terminal.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "version.h"
#include "memory.h"

#define ABT_COLS 60
#define ABT_ROWS 15

static uint8_t abt_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

/* One-time light-grey dialog background, painted once at window-open instead
   of every redraw (see centered()'s comment for why repeating this caused
   flicker). */
static void fill_background(void)
{
    uint8_t bg = abt_color(VGA_BLACK, VGA_LIGHT_GREY);
    terminal_setcolor(bg);
    for (int r = 0; r < ABT_ROWS; r++) {
        terminal_setpos(0, (size_t)r);
        for (int x = 0; x < ABT_COLS; x++) terminal_putchar(' ');
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

/* Pads the whole row width in one pass (instead of a separate full-area
   clear before redrawing) so a window compositor snapshot taken mid-update
   never catches a blanked-but-not-yet-redrawn row, which is what caused the
   visible flicker. */
static void centered(int y, const char *s, uint8_t color)
{
    int len = (int)strlen(s);
    int x = (ABT_COLS - len) / 2;
    if (x < 0) x = 0;

    terminal_setcolor(color);
    terminal_setpos(0, (size_t)y);
    int i = 0;
    for (; i < x; i++) terminal_putchar(' ');
    for (int j = 0; j < len; j++) terminal_putchar(s[j]);
    for (i = x + len; i < ABT_COLS; i++) terminal_putchar(' ');
}

static void draw_about(void)
{
    uint8_t normal = abt_color(VGA_BLACK, VGA_LIGHT_GREY);
    uint8_t dim = abt_color(VGA_DARK_GREY, VGA_LIGHT_GREY);
    uint8_t hi = abt_color(VGA_WHITE, VGA_BLUE);

    centered(1, " tOS ", hi);
    centered(3, TOS_VERSION_STRING, normal);
    centered(4, "Build: " __DATE__ " " __TIME__, dim);

    centered(6, "tOS - talOS", normal);
    centered(7, "License: GNU AGPL v3", dim);

    uint32_t total_kb = 0, used_kb = 0;
    memory_get_usage(&total_kb, &used_kb);

    char line[48];
    int k = 0;
    const char *p1 = "Memory: ";
    while (*p1) line[k++] = *p1++;
    k += fmt_uint(line + k, used_kb);
    const char *p2 = " KB used / ";
    p1 = p2;
    while (*p1) line[k++] = *p1++;
    k += fmt_uint(line + k, total_kb);
    const char *p3 = " KB total";
    p1 = p3;
    while (*p1) line[k++] = *p1++;
    line[k] = 0;

    centered(9, line, normal);
}

void about_run(void)
{
    terminal_clear();
    fill_background();

    uint32_t last_redraw = 0;

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        uint32_t now = task_get_ticks();
        if (now - last_redraw >= 20) {
            last_redraw = now;
            draw_about();
        }

        task_yield();
    }
}
