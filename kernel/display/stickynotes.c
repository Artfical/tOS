#include "stickynotes.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"

/* A small homage to the classic Mac OS "Note Pad" desk accessory: a
 * fixed 8-page yellow paper pad, one page visible at a time, flipped
 * by clicking the folded-corner ("dog-ear") glyph in the bottom-right
 * corner -- exactly like the original, which had no keyboard shortcut
 * for turning pages either. */

#define SN_PAGES   8
#define SN_COLS    79
#define SN_ROWS    22
#define HEADER_ROW 0
#define RULE_ROW   1
#define TEXT_Y0    2
#define TEXT_H     (SN_ROWS - TEXT_Y0 - 1)
#define TEXT_X0    3
#define TEXT_W     (SN_COLS - TEXT_X0 - 2)

#define SAVE_PATH "/.stickynotes"

static char sn_lines[SN_PAGES][TEXT_H][TEXT_W + 1];
static int  sn_line_len[SN_PAGES][TEXT_H];
static int  sn_line_count[SN_PAGES];
static int  cur_page;
static int  cur_row, cur_col;

static uint8_t sn_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void put_char_at(int x, int y, char c, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar(c);
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

static void reset_pad(void)
{
    memset(sn_lines, 0, sizeof(sn_lines));
    memset(sn_line_len, 0, sizeof(sn_line_len));
    for (int p = 0; p < SN_PAGES; p++) sn_line_count[p] = 1;
    cur_page = 0;
    cur_row = 0;
    cur_col = 0;
}

/* Binary format, one page after another: uint8 line_count, then for
 * each line a uint8 length followed by that many raw bytes. Bounded
 * and simple, with no delimiter that user text could collide with. */
static void save_notes(void)
{
    uint8_t buf[SN_PAGES * (1 + TEXT_H * (1 + TEXT_W))];
    uint32_t n = 0;
    for (int p = 0; p < SN_PAGES; p++) {
        buf[n++] = (uint8_t)sn_line_count[p];
        for (int r = 0; r < sn_line_count[p]; r++) {
            uint8_t len = (uint8_t)sn_line_len[p][r];
            buf[n++] = len;
            memcpy(buf + n, sn_lines[p][r], len);
            n += len;
        }
    }
    if (!fsbridge_exists(SAVE_PATH)) fsbridge_create(SAVE_PATH);
    fsbridge_write(SAVE_PATH, buf, n, 0);
}

static void load_notes(void)
{
    reset_pad();
    if (!fsbridge_exists(SAVE_PATH)) return;
    uint32_t sz = fsbridge_size(SAVE_PATH);
    if (sz == 0) return;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return;
    fsbridge_read(SAVE_PATH, buf, sz, 0);

    uint32_t n = 0;
    for (int p = 0; p < SN_PAGES && n < sz; p++) {
        int lc = buf[n++];
        if (lc < 1) lc = 1;
        if (lc > TEXT_H) lc = TEXT_H;
        sn_line_count[p] = lc;
        for (int r = 0; r < lc && n < sz; r++) {
            uint8_t len = buf[n++];
            if (len > TEXT_W) len = TEXT_W;
            if (n + len > sz) len = (uint8_t)(sz - n);
            memcpy(sn_lines[p][r], buf + n, len);
            sn_lines[p][r][len] = 0;
            sn_line_len[p][r] = len;
            n += len;
        }
    }
    free(buf);
}

static void insert_char(char c)
{
    char *line = sn_lines[cur_page][cur_row];
    int *len = &sn_line_len[cur_page][cur_row];
    if (*len >= TEXT_W) return;
    for (int i = *len; i > cur_col; i--) line[i] = line[i - 1];
    line[cur_col] = c;
    (*len)++;
    line[*len] = 0;
    cur_col++;
}

static void insert_newline(void)
{
    int *count = &sn_line_count[cur_page];
    if (*count >= TEXT_H) return;
    for (int r = *count; r > cur_row + 1; r--) {
        strcpy(sn_lines[cur_page][r], sn_lines[cur_page][r - 1]);
        sn_line_len[cur_page][r] = sn_line_len[cur_page][r - 1];
    }
    char *line = sn_lines[cur_page][cur_row];
    int tail_len = sn_line_len[cur_page][cur_row] - cur_col;
    strcpy(sn_lines[cur_page][cur_row + 1], line + cur_col);
    sn_line_len[cur_page][cur_row + 1] = tail_len;
    line[cur_col] = 0;
    sn_line_len[cur_page][cur_row] = cur_col;
    (*count)++;
    cur_row++;
    cur_col = 0;
}

static void backspace(void)
{
    char *line = sn_lines[cur_page][cur_row];
    int *len = &sn_line_len[cur_page][cur_row];
    if (cur_col > 0) {
        for (int i = cur_col - 1; i < *len - 1; i++) line[i] = line[i + 1];
        (*len)--;
        line[*len] = 0;
        cur_col--;
    } else if (cur_row > 0) {
        int prev_len = sn_line_len[cur_page][cur_row - 1];
        int merged_len = prev_len + *len;
        if (merged_len <= TEXT_W) {
            strcpy(sn_lines[cur_page][cur_row - 1] + prev_len, line);
            sn_line_len[cur_page][cur_row - 1] = merged_len;
            int count = sn_line_count[cur_page];
            for (int r = cur_row; r < count - 1; r++) {
                strcpy(sn_lines[cur_page][r], sn_lines[cur_page][r + 1]);
                sn_line_len[cur_page][r] = sn_line_len[cur_page][r + 1];
            }
            sn_lines[cur_page][count - 1][0] = 0;
            sn_line_len[cur_page][count - 1] = 0;
            sn_line_count[cur_page]--;
            cur_row--;
            cur_col = prev_len;
        }
    }
}

static void next_page(void)
{
    cur_page = (cur_page + 1) % SN_PAGES;
    cur_row = 0;
    cur_col = 0;
    if (sn_line_count[cur_page] < 1) sn_line_count[cur_page] = 1;
}

static void redraw(void)
{
    uint8_t paper = sn_color(VGA_BLACK, VGA_LIGHT_BROWN);
    for (int y = 0; y < SN_ROWS; y++) {
        terminal_setpos(0, (size_t)y);
        terminal_setcolor(paper);
        for (int x = 0; x < SN_COLS; x++) terminal_putchar(' ');
    }

    put_str(2, HEADER_ROW, "Note Pad", paper);

    char pg[16];
    int k = 0;
    const char *p = "Page ";
    while (*p) pg[k++] = *p++;
    k += fmt_uint(pg + k, (unsigned int)(cur_page + 1));
    p = " of "; while (*p) pg[k++] = *p++;
    k += fmt_uint(pg + k, (unsigned int)SN_PAGES);
    pg[k] = 0;
    put_str(SN_COLS - k - 2, HEADER_ROW, pg, paper);

    uint8_t rule = sn_color(VGA_BROWN, VGA_LIGHT_BROWN);
    for (int x = 0; x < SN_COLS; x++) put_char_at(x, RULE_ROW, 0xC4, rule);

    for (int r = 0; r < sn_line_count[cur_page]; r++) {
        put_str(TEXT_X0, TEXT_Y0 + r, sn_lines[cur_page][r], paper);
    }

    /* Folded-corner "dog-ear" in the bottom-right, like the original --
     * clicking anywhere in it flips to the next page. */
    uint8_t curl = sn_color(VGA_BROWN, VGA_LIGHT_BROWN);
    put_char_at(SN_COLS - 1, SN_ROWS - 3, 0xB0, curl);
    put_char_at(SN_COLS - 2, SN_ROWS - 2, 0xB0, curl);
    put_char_at(SN_COLS - 1, SN_ROWS - 2, 0xB1, curl);
    put_char_at(SN_COLS - 3, SN_ROWS - 1, 0xB0, curl);
    put_char_at(SN_COLS - 2, SN_ROWS - 1, 0xB1, curl);
    put_char_at(SN_COLS - 1, SN_ROWS - 1, 0xB2, curl);

    terminal_setcolor(paper);
    terminal_setpos((size_t)(TEXT_X0 + cur_col), (size_t)(TEXT_Y0 + cur_row));
}

void stickynotes_run(void)
{
    load_notes();

    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy >= SN_ROWS - 4 && ccx >= SN_COLS - 10) {
                next_page();
            } else if (ccy >= TEXT_Y0 && ccy < TEXT_Y0 + sn_line_count[cur_page] && ccx >= TEXT_X0) {
                cur_row = ccy - TEXT_Y0;
                int col = ccx - TEXT_X0;
                if (col > sn_line_len[cur_page][cur_row]) col = sn_line_len[cur_page][cur_row];
                cur_col = col;
            }
            redraw();
            task_yield();
            continue;
        }

        int spec = keyboard_get_special();
        if (spec) {
            if (spec == 1) {
                if (cur_col > 0) cur_col--;
                else if (cur_row > 0) { cur_row--; cur_col = sn_line_len[cur_page][cur_row]; }
            } else if (spec == 2) {
                if (cur_col < sn_line_len[cur_page][cur_row]) cur_col++;
                else if (cur_row < sn_line_count[cur_page] - 1) { cur_row++; cur_col = 0; }
            } else if (spec == 3) {
                if (cur_row > 0) { cur_row--; if (cur_col > sn_line_len[cur_page][cur_row]) cur_col = sn_line_len[cur_page][cur_row]; }
            } else if (spec == 4) {
                if (cur_row < sn_line_count[cur_page] - 1) { cur_row++; if (cur_col > sn_line_len[cur_page][cur_row]) cur_col = sn_line_len[cur_page][cur_row]; }
            }
            redraw();
            task_yield();
            continue;
        }

        if (!keyboard_data_available()) { task_yield(); continue; }

        char c = keyboard_getchar();
        if (c == '\n') { insert_newline(); save_notes(); redraw(); }
        else if (c == '\b' || c == 127) { backspace(); save_notes(); redraw(); }
        else if ((unsigned char)c >= ' ' && c < 127) { insert_char(c); save_notes(); redraw(); }

        task_yield();
    }
}
