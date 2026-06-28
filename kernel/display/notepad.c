#include "notepad.h"
#include "terminal.h"
#include "keyboard.h"
#include "fsbridge.h"
#include "string.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"
#include "memory.h"

#define NP_ROWS 20
#define NP_COLS 79
#define BUTTON_ROW 20
#define STATUS_ROW 21
#define SCROLLBAR_X NP_COLS
#define MAX_LINES 1000

#define NUM_BUTTONS 3
static const char *btn_labels[NUM_BUTTONS] = { "New", "Open", "Save" };
static int btn_x0[NUM_BUTTONS], btn_x1[NUM_BUTTONS];

static char lines[MAX_LINES][NP_COLS + 1];
static int line_len[MAX_LINES];
static int num_lines;
static int cur_row, cur_col;
static int view_top;
static char filename[64];
static int modified;
static char pending_path[64];
static int has_pending;

void notepad_open_path(const char *path)
{
    strncpy(pending_path, path, sizeof(pending_path) - 1);
    pending_path[sizeof(pending_path) - 1] = 0;
    has_pending = 1;
}

static uint8_t np_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static int fmt_uint(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void ensure_visible(void)
{
    if (cur_row < view_top) view_top = cur_row;
    if (cur_row >= view_top + NP_ROWS) view_top = cur_row - NP_ROWS + 1;
    int max_top = num_lines - NP_ROWS;
    if (max_top < 0) max_top = 0;
    if (view_top > max_top) view_top = max_top;
    if (view_top < 0) view_top = 0;
}

static void reset_buffer(void)
{
    num_lines = 1;
    cur_row = 0;
    cur_col = 0;
    view_top = 0;
    modified = 0;
    filename[0] = 0;
    for (int i = 0; i < MAX_LINES; i++) { lines[i][0] = 0; line_len[i] = 0; }
}

static void status_line(const char *msg)
{
    terminal_setpos(0, STATUS_ROW);
    terminal_setcolor(np_color(VGA_BLACK, VGA_LIGHT_GREY));
    int i = 0;
    for (; msg[i] && i < NP_COLS; i++) terminal_putchar(msg[i]);
    for (; i < NP_COLS; i++) terminal_putchar(' ');
    terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
}

static void draw_buttons(void)
{
    terminal_setpos(0, BUTTON_ROW);
    terminal_setcolor(np_color(VGA_BLACK, VGA_LIGHT_GREY));
    for (int i = 0; i < NP_COLS; i++) terminal_putchar(' ');

    int x = 2;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        const char *lbl = btn_labels[i];
        int len = (int)strlen(lbl);
        int w = len + 4; /* "[ " + label + " ]" */
        btn_x0[i] = x;
        btn_x1[i] = x + w - 1;

        terminal_setpos((size_t)x, BUTTON_ROW);
        terminal_setcolor(np_color(VGA_WHITE, VGA_BLUE));
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 2;
    }
    terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
}

static void draw_scrollbar(void)
{
    int can_scroll = num_lines > NP_ROWS;
    int max_top = can_scroll ? num_lines - NP_ROWS : 0;
    int thumb_row = (can_scroll && max_top > 0) ? (view_top * (NP_ROWS - 1)) / max_top : 0;

    for (int r = 0; r < NP_ROWS; r++) {
        terminal_setpos((size_t)SCROLLBAR_X, (size_t)r);
        if (!can_scroll) {
            terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
            terminal_putchar(' ');
        } else if (r == thumb_row) {
            terminal_setcolor(np_color(VGA_WHITE, VGA_BLUE));
            terminal_putchar((char)0xDB);
        } else {
            terminal_setcolor(np_color(VGA_DARK_GREY, VGA_BLACK));
            terminal_putchar((char)0xB1);
        }
    }
}

static void redraw(void)
{
    ensure_visible();

    terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
    for (int r = 0; r < NP_ROWS; r++) {
        int src = view_top + r;
        terminal_setpos(0, r);
        for (int c = 0; c < NP_COLS; c++) {
            char ch = (src < num_lines && c < line_len[src]) ? lines[src][c] : ' ';
            terminal_putchar(ch);
        }
    }

    draw_scrollbar();
    draw_buttons();

    char status[NP_COLS + 1];
    int k = 0;
    const char *name = filename[0] ? filename : "Untitled";
    while (name[k] && k < 40) { status[k] = name[k]; k++; }
    if (modified) status[k++] = '*';
    status[k++] = ' '; status[k++] = '(';
    k += fmt_uint(status + k, (uint32_t)(cur_row + 1));
    status[k++] = '/';
    k += fmt_uint(status + k, (uint32_t)num_lines);
    status[k++] = ')';
    status[k] = 0;
    status_line(status);

    terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
    terminal_setpos((size_t)cur_col, (size_t)(cur_row - view_top));
}

static void ensure_txt_extension(char *name, int max)
{
    int len = (int)strlen(name);
    if (len == 0) return;
    for (int i = 0; i < len; i++) if (name[i] == '.') return;
    if (len + 4 < max) strcat(name, ".txt");
}

static void save_file(const char *fname)
{
    if (fsbridge_exists(fname)) fsbridge_delete(fname);
    if (fsbridge_create(fname) != 0) return;

    uint32_t offset = 0;
    for (int r = 0; r < num_lines; r++) {
        if (line_len[r] > 0) {
            fsbridge_write(fname, lines[r], (uint32_t)line_len[r], offset);
            offset += (uint32_t)line_len[r];
        }
        if (r < num_lines - 1) {
            fsbridge_write(fname, "\n", 1, offset);
            offset++;
        }
    }

    strncpy(filename, fname, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;
    modified = 0;
}

static void load_file(const char *fname)
{
    reset_buffer();
    strncpy(filename, fname, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;

    if (!fsbridge_exists(fname)) return;

    uint32_t sz = fsbridge_size(fname);
    char *buf = (char *)malloc(sz ? sz : 1);
    if (!buf) return;
    fsbridge_read(fname, buf, sz, 0);

    int r = 0, c = 0;
    for (uint32_t i = 0; i < sz && r < MAX_LINES - 1; i++) {
        if (buf[i] == '\n') {
            line_len[r] = c;
            lines[r][c] = 0;
            r++; c = 0;
        } else if (c < NP_COLS) {
            lines[r][c++] = buf[i];
        }
    }
    line_len[r] = c;
    lines[r][c] = 0;
    num_lines = r + 1;
    free(buf);
}

static void prompt_filename(const char *prompt, char *buf, int max)
{
    status_line(prompt);
    terminal_setpos((size_t)strlen(prompt), STATUS_ROW);
    keyboard_readline(buf, max);
}

static void do_save(void)
{
    char fname[64];
    if (filename[0]) {
        save_file(filename);
    } else {
        prompt_filename("Save as: ", fname, sizeof(fname));
        if (fname[0]) {
            ensure_txt_extension(fname, sizeof(fname));
            save_file(fname);
        }
    }
}

static void do_open(void)
{
    char fname[64];
    prompt_filename("Open: ", fname, sizeof(fname));
    if (fname[0]) {
        ensure_txt_extension(fname, sizeof(fname));
        load_file(fname);
    }
}

static void do_new(void)
{
    reset_buffer();
    terminal_clear();
}

static void insert_char(char c)
{
    if (line_len[cur_row] >= NP_COLS) return;
    for (int i = line_len[cur_row]; i > cur_col; i--) lines[cur_row][i] = lines[cur_row][i - 1];
    lines[cur_row][cur_col] = c;
    line_len[cur_row]++;
    lines[cur_row][line_len[cur_row]] = 0;
    cur_col++;
    modified = 1;
}

static void insert_newline(void)
{
    if (num_lines >= MAX_LINES) return;
    for (int r = num_lines; r > cur_row + 1; r--) {
        strcpy(lines[r], lines[r - 1]);
        line_len[r] = line_len[r - 1];
    }
    int tail_len = line_len[cur_row] - cur_col;
    strcpy(lines[cur_row + 1], lines[cur_row] + cur_col);
    line_len[cur_row + 1] = tail_len;
    lines[cur_row][cur_col] = 0;
    line_len[cur_row] = cur_col;
    num_lines++;
    cur_row++;
    cur_col = 0;
    modified = 1;
}

static void backspace(void)
{
    if (cur_col > 0) {
        for (int i = cur_col - 1; i < line_len[cur_row] - 1; i++) lines[cur_row][i] = lines[cur_row][i + 1];
        line_len[cur_row]--;
        lines[cur_row][line_len[cur_row]] = 0;
        cur_col--;
        modified = 1;
    } else if (cur_row > 0) {
        int prev_len = line_len[cur_row - 1];
        int merged_len = prev_len + line_len[cur_row];
        if (merged_len <= NP_COLS) {
            strcpy(lines[cur_row - 1] + prev_len, lines[cur_row]);
            line_len[cur_row - 1] = merged_len;
            for (int r = cur_row; r < num_lines - 1; r++) {
                strcpy(lines[r], lines[r + 1]);
                line_len[r] = line_len[r + 1];
            }
            lines[num_lines - 1][0] = 0;
            line_len[num_lines - 1] = 0;
            num_lines--;
            cur_row--;
            cur_col = prev_len;
            modified = 1;
        }
    }
}

void notepad_run(void)
{
    reset_buffer();
    if (has_pending) {
        load_file(pending_path);
        has_pending = 0;
    }
    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();

        int action = wm_get_menu_action();
        if (action) {
            if (action == WM_ACTION_NEW) do_new();
            else if (action == WM_ACTION_OPEN) do_open();
            else if (action == WM_ACTION_SAVE) do_save();
            redraw();
            continue;
        }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == BUTTON_ROW) {
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (ccx >= btn_x0[i] && ccx <= btn_x1[i]) {
                        if (i == 0) do_new();
                        else if (i == 1) do_open();
                        else if (i == 2) do_save();
                        break;
                    }
                }
                redraw();
            } else if (ccx == SCROLLBAR_X && ccy >= 0 && ccy < NP_ROWS && num_lines > NP_ROWS) {
                int max_top = num_lines - NP_ROWS;
                int target = (NP_ROWS > 1) ? (ccy * max_top) / (NP_ROWS - 1) : 0;
                if (target < 0) target = 0;
                if (target > max_top) target = max_top;
                cur_row = target;
                if (cur_row >= num_lines) cur_row = num_lines - 1;
                if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                redraw();
            }
            continue;
        }

        int spec = keyboard_get_special();
        if (spec) {
            if (spec == 1) { if (cur_col > 0) cur_col--; }
            else if (spec == 2) { if (cur_col < line_len[cur_row]) cur_col++; }
            else if (spec == 3) { if (cur_row > 0) { cur_row--; if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row]; } }
            else if (spec == 4) { if (cur_row < num_lines - 1) { cur_row++; if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row]; } }
            redraw();
            continue;
        }

        if (!keyboard_data_available() || !wm_current_task_has_focus()) {
            task_yield();
            continue;
        }

        char c = keyboard_getchar();

        if (c == 0x13) {
            do_save();
            redraw();
        } else if (c == 0x0F) {
            do_open();
            redraw();
        } else if (c == 0x0E) {
            do_new();
            redraw();
        } else if (c == '\n') {
            insert_newline();
            redraw();
        } else if (c == '\b' || c == 127) {
            backspace();
            redraw();
        } else if ((unsigned char)c >= ' ' && c < 127) {
            insert_char(c);
            redraw();
        }
    }
}
