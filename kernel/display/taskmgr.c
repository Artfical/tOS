#include "taskmgr.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "memory.h"
#include "gui.h"
#include "wm.h"
#include "string.h"

#define TM_COLS 79
#define TM_ROWS 22
#define LIST_Y0 3
#define STATUS_Y (TM_ROWS - 1)
#define LIST_VISIBLE (STATUS_Y - LIST_Y0)

static uint32_t row_pid[MAX_TASKS];
static char row_name[MAX_TASKS][TASK_NAME_MAX];
static uint32_t row_state[MAX_TASKS];
static int row_count;
static int selected;
static int scroll_off;
static char status_msg[TM_COLS + 1];

static int btn_x0, btn_x1;

static uint8_t tm_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void clear_area(void)
{
    uint8_t bg = tm_color(VGA_BLACK, VGA_LIGHT_GREY);
    char blank[TM_COLS + 1];
    for (int i = 0; i < TM_COLS; i++) blank[i] = ' ';
    blank[TM_COLS] = 0;
    for (int r = 0; r < TM_ROWS; r++) put_str(0, r, blank, bg);
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

static void collect_cb(uint32_t pid, const char *name, uint32_t state)
{
    if (row_count >= MAX_TASKS) return;
    row_pid[row_count] = pid;
    strncpy(row_name[row_count], name, TASK_NAME_MAX - 1);
    row_name[row_count][TASK_NAME_MAX - 1] = 0;
    row_state[row_count] = state;
    row_count++;
}

static const char *state_name(uint32_t state)
{
    switch (state) {
        case TASK_STATE_READY:    return "READY";
        case TASK_STATE_RUNNING:  return "RUN";
        case TASK_STATE_SLEEPING: return "SLEEP";
        case TASK_STATE_ZOMBIE:   return "ZOMBIE";
        default: return "?";
    }
}

static void refresh_tasks(void)
{
    row_count = 0;
    task_foreach(collect_cb);
    if (selected >= row_count) selected = row_count > 0 ? row_count - 1 : 0;
    if (selected < 0) selected = 0;
    if (selected < scroll_off) scroll_off = selected;
    if (selected >= scroll_off + LIST_VISIBLE) scroll_off = selected - LIST_VISIBLE + 1;
    if (scroll_off < 0) scroll_off = 0;
}

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < TM_COLS) { status_msg[i] = s[i]; i++; }
    status_msg[i] = 0;
}

static void draw_header(void)
{
    put_str(0, 0, " Task Manager - Up/Dn select, K kill, click a row                              ",
            tm_color(VGA_WHITE, VGA_BLUE));

    uint32_t total_kb = 0, used_kb = 0;
    memory_get_usage(&total_kb, &used_kb);
    uint32_t ticks = task_get_ticks();
    uint32_t sec = ticks / 100;
    uint32_t hr = sec / 3600, min = (sec / 60) % 60, s = sec % 60;

    char line[TM_COLS + 1];
    int k = 0;
    const char *p = "Tasks: ";
    while (*p) line[k++] = *p++;
    k += fmt_uint(line + k, (uint32_t)row_count);
    p = "  Up: "; while (*p) line[k++] = *p++;
    k += fmt_uint(line + k, hr); line[k++] = 'h'; line[k++] = ' ';
    k += fmt_uint(line + k, min); line[k++] = 'm'; line[k++] = ' ';
    k += fmt_uint(line + k, s); line[k++] = 's';
    p = "  Mem: "; while (*p) line[k++] = *p++;
    k += fmt_uint(line + k, used_kb);
    p = "K/"; while (*p) line[k++] = *p++;
    k += fmt_uint(line + k, total_kb);
    line[k++] = 'K';
    while (k < TM_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, 1, line, tm_color(VGA_BLACK, VGA_LIGHT_GREY));

    put_str(0, 2, " PID   NAME                          STATE                                     ",
            tm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));

    const char *lbl = "Kill";
    int len = (int)strlen(lbl);
    int w = len + 4;
    btn_x0 = TM_COLS - w - 1;
    btn_x1 = btn_x0 + w - 1;
    terminal_setpos((size_t)btn_x0, 2);
    terminal_setcolor(tm_color(VGA_WHITE, VGA_RED));
    terminal_putchar('[');
    terminal_putchar(' ');
    for (int i = 0; i < len; i++) terminal_putchar(lbl[i]);
    terminal_putchar(' ');
    terminal_putchar(']');
}

static void draw_list(void)
{
    for (int row = 0; row < LIST_VISIBLE; row++) {
        int idx = scroll_off + row;
        int y = LIST_Y0 + row;
        char line[TM_COLS + 1];
        for (int c = 0; c < TM_COLS; c++) line[c] = ' ';
        line[TM_COLS] = 0;

        if (idx < row_count) {
            int k = 0;
            line[k++] = (idx == selected) ? '>' : ' ';
            line[k++] = ' ';
            k += fmt_uint(line + k, row_pid[idx]);
            while (k < 8) line[k++] = ' ';
            int j = 0;
            while (row_name[idx][j] && k < 38) line[k++] = row_name[idx][j++];
            while (k < 38) line[k++] = ' ';
            const char *st = state_name(row_state[idx]);
            int sj = 0;
            while (st[sj] && k < TM_COLS) line[k++] = st[sj++];
            while (k < TM_COLS) line[k++] = ' ';
        }
        line[TM_COLS] = 0;

        uint8_t color = (idx == selected && idx < row_count)
            ? tm_color(VGA_WHITE, VGA_BLUE)
            : tm_color(VGA_BLACK, VGA_LIGHT_GREY);
        put_str(0, y, line, color);
    }
}

static void draw_status(void)
{
    char line[TM_COLS + 1];
    int k = 0;
    while (status_msg[k] && k < TM_COLS) { line[k] = status_msg[k]; k++; }
    while (k < TM_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_Y, line, tm_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void redraw(void)
{
    refresh_tasks();
    draw_header();
    draw_list();
    draw_status();
}

static void do_kill(void)
{
    if (row_count == 0 || selected >= row_count) return;
    uint32_t pid = row_pid[selected];

    if (pid == 0) { set_status("Can't kill the idle task."); return; }
    if (pid == task_get_pid()) { set_status("Can't kill Task Manager's own task."); return; }

    char confirm[TM_COLS + 1];
    int k = 0;
    const char *p = "Kill PID ";
    while (*p) confirm[k++] = *p++;
    k += fmt_uint(confirm + k, pid);
    p = " (";
    while (*p) confirm[k++] = *p++;
    int j = 0;
    while (row_name[selected][j] && k < TM_COLS - 8) confirm[k++] = row_name[selected][j++];
    p = ")? (y/n) ";
    while (*p) confirm[k++] = *p++;
    confirm[k] = 0;

    put_str(0, STATUS_Y, "                                                                               ",
            tm_color(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_Y, confirm, tm_color(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)k, (size_t)STATUS_Y);

    if (!keyboard_yesno()) { status_msg[0] = 0; return; }

    if (task_kill(pid) == 0) set_status("Killed.");
    else set_status("Kill failed (cannot kill this task).");
}

void taskmgr_run(void)
{
    selected = 0;
    scroll_off = 0;
    status_msg[0] = 0;
    terminal_clear();
    clear_area();

    uint32_t last_redraw = 0;

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == 2 && ccx >= btn_x0 && ccx <= btn_x1) {
                do_kill();
            } else if (ccy >= LIST_Y0 && ccy < STATUS_Y) {
                int idx = scroll_off + (ccy - LIST_Y0);
                if (idx < row_count) selected = idx;
            }
            redraw();
            task_yield();
            continue;
        }

        int spec = keyboard_get_special();
        if (spec == 3) { if (selected > 0) selected--; redraw(); task_yield(); continue; }
        if (spec == 4) { if (selected < row_count - 1) selected++; redraw(); task_yield(); continue; }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == 'k' || c == 'K') { do_kill(); redraw(); task_yield(); continue; }
        }

        uint32_t now = task_get_ticks();
        if (now - last_redraw >= 25) {
            last_redraw = now;
            redraw();
        }

        task_yield();
    }
}
