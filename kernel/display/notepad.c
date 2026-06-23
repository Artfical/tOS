#include "notepad.h"
#include "terminal.h"
#include "keyboard.h"
#include "ramfs.h"
#include "string.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"

#define NP_ROWS 22
#define NP_COLS 79
#define STATUS_ROW 22

static char lines[NP_ROWS][NP_COLS + 1];
static int line_len[NP_ROWS];
static int num_lines;
static int cur_row, cur_col;
static char filename[64];
static int modified;

static uint8_t np_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void reset_buffer(void)
{
    num_lines = 1;
    cur_row = 0;
    cur_col = 0;
    modified = 0;
    filename[0] = 0;
    for (int i = 0; i < NP_ROWS; i++) { lines[i][0] = 0; line_len[i] = 0; }
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

static void redraw(void)
{
    for (int r = 0; r < NP_ROWS; r++) {
        terminal_setpos(0, r);
        for (int c = 0; c < NP_COLS; c++)
            terminal_putchar(c < line_len[r] ? lines[r][c] : ' ');
    }

    char status[NP_COLS + 1];
    int k = 0;
    const char *name = filename[0] ? filename : "Untitled";
    while (name[k] && k < 40) { status[k] = name[k]; k++; }
    if (modified) status[k++] = '*';
    const char *hint = "  ^S Save  ^O Open";
    int h = 0;
    while (hint[h] && k < NP_COLS) status[k++] = hint[h++];
    status[k] = 0;
    status_line(status);

    terminal_setpos((size_t)cur_col, (size_t)cur_row);
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
    if (ramfs_exists(fname)) ramfs_delete(fname);
    if (ramfs_create(fname) != 0) return;

    uint32_t offset = 0;
    for (int r = 0; r < num_lines; r++) {
        if (line_len[r] > 0) {
            ramfs_write(fname, lines[r], (uint32_t)line_len[r], offset);
            offset += (uint32_t)line_len[r];
        }
        if (r < num_lines - 1) {
            ramfs_write(fname, "\n", 1, offset);
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

    if (!ramfs_exists(fname)) return;

    uint32_t sz = ramfs_size(fname);
    char buf[NP_ROWS * (NP_COLS + 1)];
    if (sz >= sizeof(buf)) sz = sizeof(buf) - 1;
    ramfs_read(fname, buf, sz, 0);

    int r = 0, c = 0;
    for (uint32_t i = 0; i < sz && r < NP_ROWS; i++) {
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
}

static void prompt_filename(const char *prompt, char *buf, int max)
{
    status_line(prompt);
    terminal_setpos((size_t)strlen(prompt), STATUS_ROW);
    keyboard_readline(buf, max);
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
    if (num_lines >= NP_ROWS) return;
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
    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();
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
            redraw();
        } else if (c == 0x0F) {
            char fname[64];
            prompt_filename("Open: ", fname, sizeof(fname));
            if (fname[0]) {
                ensure_txt_extension(fname, sizeof(fname));
                load_file(fname);
            }
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
