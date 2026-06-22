/*
 * "htop" builtin command.
 *
 * tOS has no /proc, no ncurses, and no libc, so the upstream htop
 * codebase (https://github.com/htop-dev/htop, GPL-2.0-only) cannot be
 * compiled or linked here. This file is an original, from-scratch
 * implementation written for tOS that mirrors htop's layout (a header
 * bar with system stats followed by a live-updating process table) using
 * tOS's own scheduler introspection (task_foreach/task_get_ticks/etc).
 *
 * Because the command is named and styled after htop, htop's GPL-2.0
 * license text is kept alongside this file's attribution in
 * third_party/htop/ and credited in the top-level README, even though no
 * htop source is included or compiled into tOS.
 */
#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "memory.h"
#include "scheduler.h"
#include "keyboard.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 0;
    if (n == 0) { terminal_putchar('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) terminal_putchar(buf[--i]);
}

static void print_padded(uint32_t n, int width)
{
    uint32_t tmp = n;
    int digits = 1;
    while (tmp >= 10) { tmp /= 10; digits++; }
    for (int i = digits; i < width; i++) terminal_putchar(' ');
    print_num(n);
}

#define HTOP_MAX_ROWS MAX_TASKS

static uint32_t row_pid[HTOP_MAX_ROWS];
static char row_name[HTOP_MAX_ROWS][TASK_NAME_MAX];
static uint32_t row_state[HTOP_MAX_ROWS];
static int row_count;

static void collect_cb(uint32_t pid, const char *name, uint32_t state)
{
    if (row_count >= HTOP_MAX_ROWS) return;
    row_pid[row_count] = pid;
    strncpy(row_name[row_count], name, TASK_NAME_MAX - 1);
    row_name[row_count][TASK_NAME_MAX - 1] = 0;
    row_state[row_count] = state;
    row_count++;
}

static const char *state_name(uint32_t state)
{
    switch (state) {
        case 0: return "READY";
        case 1: return "RUN";
        case 2: return "SLEEP";
        case 3: return "ZOMBIE";
        default: return "?";
    }
}

static void draw_frame(void)
{
    terminal_clear();
    terminal_setpos(0, 0);

    uint32_t total_kb = 0, used_kb = 0;
    memory_get_usage(&total_kb, &used_kb);
    uint32_t ticks = task_get_ticks();
    uint32_t sec = ticks / 100;
    uint32_t hr = sec / 3600, min = (sec / 60) % 60, s = sec % 60;

    row_count = 0;
    task_foreach(collect_cb);

    terminal_writestring("tOS htop  |  tasks: ");
    print_num((uint32_t)row_count);
    terminal_writestring("  |  uptime: ");
    print_num(hr); terminal_writestring("h ");
    print_num(min); terminal_writestring("m ");
    print_num(s); terminal_writestring("s\n");

    terminal_writestring("Mem[");
    print_num(used_kb);
    terminal_writestring("K/");
    print_num(total_kb);
    terminal_writestring("K]\n\n");

    terminal_writestring(" PID   NAME                              STATE\n");
    for (int i = 0; i < row_count; i++) {
        terminal_putchar(' ');
        print_padded(row_pid[i], 4);
        terminal_writestring("   ");
        int nlen = (int)strlen(row_name[i]);
        terminal_writestring(row_name[i]);
        for (int p = nlen; p < 34; p++) terminal_putchar(' ');
        terminal_writestring(state_name(row_state[i]));
        terminal_putchar('\n');
    }

    terminal_writestring("\nq: quit   any other key: refresh now\n");
}

void cmd_htop(int argc, char **args)
{
    (void)argc; (void)args;

    for (;;) {
        draw_frame();

        uint32_t start = task_get_ticks();
        for (;;) {
            if (keyboard_data_available()) {
                char c = keyboard_getchar();
                if (c == 'q' || c == 'Q') return;
                break;
            }
            if (task_get_ticks() - start >= 100) break;
            task_yield();
        }
    }
}
