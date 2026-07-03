#include "notepad.h"
#include "terminal.h"
#include "keyboard.h"
#include "fsbridge.h"
#include "string.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"
#include "memory.h"
#include "ctype.h"

#define NP_ROWS    20
#define NP_COLS    79
#define LINE_NUM_W 5          /* " 123|" gutter */
#define CONTENT_W  (NP_COLS - LINE_NUM_W)   /* 74 */
#define BUTTON_ROW 20
#define STATUS_ROW 21
#define SCROLLBAR_X NP_COLS
#define MAX_LINES  1000

#define NUM_BUTTONS 5
static const char *btn_labels[NUM_BUTTONS] = { "New", "Open", "Save", "Find", "Replace" };
static int btn_x0[NUM_BUTTONS], btn_x1[NUM_BUTTONS];

static char lines[MAX_LINES][CONTENT_W + 1];
static int  line_len[MAX_LINES];
static int  num_lines;
static int  cur_row, cur_col;
static int  view_top;
static char filename[64];
static char last_search[64];
static int  modified;
static char pending_path[64];
static int  has_pending;
static int  syn_highlight;

static int sel_active;
static int sel_anchor_row, sel_anchor_col;
static char clipboard[4096];

void notepad_open_path(const char *path)
{
    strncpy(pending_path, path, sizeof(pending_path) - 1);
    pending_path[sizeof(pending_path) - 1] = 0;
    has_pending = 1;
}

static uint8_t np_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void insert_char(char c);
static void insert_newline(void);

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
    syn_highlight = 0;
    sel_active = 0;
    for (int i = 0; i < MAX_LINES; i++) { lines[i][0] = 0; line_len[i] = 0; }
}

static void update_syn_mode(void)
{
    int len = (int)strlen(filename);
    syn_highlight = (len >= 2 &&
        ((filename[len-2] == '.' && (filename[len-1] == 'c' || filename[len-1] == 'h')) ||
         (len >= 4 && filename[len-4] == '.' &&
          filename[len-3] == 'c' && filename[len-2] == 'p' && filename[len-1] == 'p')));
}

/* ── syntax highlighting ────────────────────────────────────────────────── */

static const char *SYN_KW[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","inline","int","long",
    "register","return","short","signed","sizeof","static","struct","switch",
    "typedef","union","unsigned","void","volatile","while",
    "uint8_t","uint16_t","uint32_t","uint64_t",
    "int8_t","int16_t","int32_t","int64_t","size_t","NULL",
    NULL
};

static void compute_line_colors(const char *line, int len, uint8_t *colors)
{
    uint8_t def = np_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int i = 0; i < CONTENT_W; i++) colors[i] = def;
    if (!syn_highlight || len == 0) return;

    /* preprocessor lines */
    if (line[0] == '#') {
        for (int i = 0; i < len; i++) colors[i] = np_color(VGA_MAGENTA, VGA_BLACK);
        return;
    }

    int i = 0;
    while (i < len) {
        /* line comment */
        if (i < len - 1 && line[i] == '/' && line[i + 1] == '/') {
            for (int j = i; j < len; j++) colors[j] = np_color(VGA_DARK_GREY, VGA_BLACK);
            break;
        }
        /* block comment start */
        if (i < len - 1 && line[i] == '/' && line[i + 1] == '*') {
            int j = i;
            colors[j++] = np_color(VGA_DARK_GREY, VGA_BLACK);
            colors[j++] = np_color(VGA_DARK_GREY, VGA_BLACK);
            while (j < len) {
                colors[j] = np_color(VGA_DARK_GREY, VGA_BLACK);
                if (j < len - 1 && line[j] == '*' && line[j + 1] == '/') {
                    colors[++j] = np_color(VGA_DARK_GREY, VGA_BLACK);
                    j++;
                    break;
                }
                j++;
            }
            i = j;
            continue;
        }
        /* string literal */
        if (line[i] == '"') {
            colors[i] = np_color(VGA_GREEN, VGA_BLACK);
            int j = i + 1;
            while (j < len) {
                colors[j] = np_color(VGA_GREEN, VGA_BLACK);
                if (line[j] == '\\') { j++; if (j < len) { colors[j] = np_color(VGA_GREEN, VGA_BLACK); j++; } }
                else if (line[j] == '"') { j++; break; }
                else j++;
            }
            i = j;
            continue;
        }
        /* char literal */
        if (line[i] == '\'') {
            colors[i] = np_color(VGA_GREEN, VGA_BLACK);
            int j = i + 1;
            while (j < len) {
                colors[j] = np_color(VGA_GREEN, VGA_BLACK);
                if (line[j] == '\\') { j++; if (j < len) { colors[j] = np_color(VGA_GREEN, VGA_BLACK); j++; } }
                else if (line[j] == '\'') { j++; break; }
                else j++;
            }
            i = j;
            continue;
        }
        /* keyword / identifier */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int wend = i;
            while (wend < len && (isalnum((unsigned char)line[wend]) || line[wend] == '_')) wend++;
            int wlen = wend - i;
            int is_kw = 0;
            for (int k = 0; SYN_KW[k]; k++) {
                int klen = (int)strlen(SYN_KW[k]);
                if (klen == wlen && strncmp(line + i, SYN_KW[k], (size_t)wlen) == 0) { is_kw = 1; break; }
            }
            uint8_t col = is_kw ? np_color(VGA_CYAN, VGA_BLACK) : def;
            for (int j = i; j < wend; j++) colors[j] = col;
            i = wend;
            continue;
        }
        /* number */
        if (isdigit((unsigned char)line[i])) {
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '.' || line[i] == 'x'))
                colors[i++] = np_color(VGA_LIGHT_BROWN, VGA_BLACK);
            continue;
        }
        i++;
    }
}

/* ── selection helpers ──────────────────────────────────────────────────── */

static int selection_empty(void)
{
    return !sel_active || (sel_anchor_row == cur_row && sel_anchor_col == cur_col);
}

static void get_selection_range(int *sr, int *sc, int *er, int *ec)
{
    if (sel_anchor_row < cur_row || (sel_anchor_row == cur_row && sel_anchor_col <= cur_col)) {
        *sr = sel_anchor_row; *sc = sel_anchor_col; *er = cur_row; *ec = cur_col;
    } else {
        *sr = cur_row; *sc = cur_col; *er = sel_anchor_row; *ec = sel_anchor_col;
    }
}

static void copy_selection(void)
{
    if (selection_empty()) return;
    int sr, sc, er, ec;
    get_selection_range(&sr, &sc, &er, &ec);
    int k = 0;
    for (int r = sr; r <= er && k < (int)sizeof(clipboard) - 1; r++) {
        int c0 = (r == sr) ? sc : 0;
        int c1 = (r == er) ? ec : line_len[r];
        for (int c = c0; c < c1 && k < (int)sizeof(clipboard) - 1; c++) clipboard[k++] = lines[r][c];
        if (r != er && k < (int)sizeof(clipboard) - 1) clipboard[k++] = '\n';
    }
    clipboard[k] = 0;
}

static void delete_selection(void)
{
    if (selection_empty()) return;
    int sr, sc, er, ec;
    get_selection_range(&sr, &sc, &er, &ec);

    if (sr == er) {
        int tail = line_len[sr] - ec;
        memmove(lines[sr] + sc, lines[sr] + ec, (size_t)tail);
        line_len[sr] = sc + tail;
        lines[sr][line_len[sr]] = 0;
    } else {
        int prefix_len = sc;
        int suffix_len = line_len[er] - ec;
        if (prefix_len + suffix_len <= CONTENT_W) {
            memcpy(lines[sr] + prefix_len, lines[er] + ec, (size_t)suffix_len);
            line_len[sr] = prefix_len + suffix_len;
            lines[sr][line_len[sr]] = 0;

            int removed = er - sr;
            for (int r = sr + 1; r < num_lines - removed; r++) {
                strcpy(lines[r], lines[r + removed]);
                line_len[r] = line_len[r + removed];
            }
            for (int r = num_lines - removed; r < num_lines; r++) { lines[r][0] = 0; line_len[r] = 0; }
            num_lines -= removed;
        }
    }

    cur_row = sr;
    cur_col = sc;
    sel_active = 0;
    modified = 1;
}

static void cut_selection(void)
{
    copy_selection();
    delete_selection();
}

static void paste_clipboard(void)
{
    if (!clipboard[0]) return;
    if (!selection_empty()) delete_selection();
    for (int i = 0; clipboard[i]; i++) {
        if (clipboard[i] == '\n') insert_newline();
        else insert_char(clipboard[i]);
    }
}

/* ── drawing ────────────────────────────────────────────────────────────── */

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
        int w = len + 4;
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

static void draw_line_number(int screen_row, int src_line)
{
    terminal_setpos(0, (size_t)screen_row);
    terminal_setcolor(np_color(VGA_DARK_GREY, VGA_BLACK));
    if (src_line >= 0 && src_line < num_lines) {
        int n = src_line + 1;
        char buf[5];
        buf[4] = '|';
        for (int d = 3; d >= 0; d--) {
            buf[d] = (n > 0) ? (char)('0' + n % 10) : ' ';
            n /= 10;
        }
        for (int d = 0; d < 5; d++) terminal_putchar(buf[d]);
    } else {
        terminal_putchar(' ');
        terminal_putchar(' ');
        terminal_putchar(' ');
        terminal_putchar(' ');
        terminal_putchar('|');
    }
}

static void redraw(void)
{
    ensure_visible();

    int has_sel = !selection_empty();
    int sr = 0, sc = 0, er = 0, ec = 0;
    if (has_sel) get_selection_range(&sr, &sc, &er, &ec);

    uint8_t syn_colors[CONTENT_W];

    for (int r = 0; r < NP_ROWS; r++) {
        int src = view_top + r;

        /* line number gutter */
        draw_line_number(r, src < num_lines ? src : -1);

        /* content area */
        if (src < num_lines)
            compute_line_colors(lines[src], line_len[src], syn_colors);

        terminal_setpos((size_t)LINE_NUM_W, (size_t)r);
        for (int c = 0; c < CONTENT_W; c++) {
            char ch = (src < num_lines && c < line_len[src]) ? lines[src][c] : ' ';
            int selected = 0;
            if (has_sel) {
                if (src > sr && src < er) selected = 1;
                else if (src == sr && src == er) selected = (c >= sc && c < ec);
                else if (src == sr && src != er) selected = (c >= sc);
                else if (src == er && src != sr) selected = (c < ec);
            }
            if (selected)
                terminal_setcolor(np_color(VGA_WHITE, VGA_BLUE));
            else
                terminal_setcolor((src < num_lines) ? syn_colors[c] : np_color(VGA_LIGHT_GREY, VGA_BLACK));
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
    if (syn_highlight) { const char *tag = " [C]"; while (*tag) status[k++] = *tag++; }
    status[k++] = ' '; status[k++] = '(';
    k += fmt_uint(status + k, (uint32_t)(cur_row + 1));
    status[k++] = ':';
    k += fmt_uint(status + k, (uint32_t)(cur_col + 1));
    status[k++] = '/';
    k += fmt_uint(status + k, (uint32_t)num_lines);
    status[k++] = ')';
    status[k] = 0;
    status_line(status);

    terminal_setcolor(np_color(VGA_LIGHT_GREY, VGA_BLACK));
    terminal_setpos((size_t)(cur_col + LINE_NUM_W), (size_t)(cur_row - view_top));
}

/* ── file ops ───────────────────────────────────────────────────────────── */

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
    update_syn_mode();
}

static void load_file(const char *fname)
{
    reset_buffer();
    strncpy(filename, fname, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;
    update_syn_mode();

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
        } else if (c < CONTENT_W) {
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

static int find_in_buffer(const char *needle, int start_row, int start_col, int *out_row, int *out_col)
{
    int nlen = (int)strlen(needle);
    if (nlen == 0 || num_lines == 0) return 0;

    int row = start_row;
    int col = start_col;
    for (int scanned = 0; scanned <= num_lines; scanned++) {
        for (int c = col; c <= line_len[row] - nlen; c++) {
            if (strncmp(lines[row] + c, needle, (size_t)nlen) == 0) {
                *out_row = row;
                *out_col = c;
                return 1;
            }
        }
        row = (row + 1) % num_lines;
        col = 0;
    }
    return 0;
}

static void do_find(void)
{
    sel_active = 0;
    char term[64];
    prompt_filename("Find: ", term, sizeof(term));
    if (term[0]) {
        strncpy(last_search, term, sizeof(last_search) - 1);
        last_search[sizeof(last_search) - 1] = 0;
    }
    if (!last_search[0]) { redraw(); return; }

    int fr, fc;
    if (find_in_buffer(last_search, cur_row, cur_col + 1, &fr, &fc)) {
        cur_row = fr;
        cur_col = fc;
        redraw();
        status_line("Found.");
    } else {
        redraw();
        status_line("Not found.");
    }
}

static int replace_in_line(int row, const char *term, const char *repl)
{
    int tlen = (int)strlen(term);
    int rlen = (int)strlen(repl);
    if (tlen == 0) return 0;

    int count = 0;
    int col = 0;
    while (col <= line_len[row] - tlen) {
        if (strncmp(lines[row] + col, term, (size_t)tlen) != 0) { col++; continue; }

        int newlen = line_len[row] - tlen + rlen;
        if (newlen > CONTENT_W) { col++; continue; }

        int tail_len = line_len[row] - (col + tlen);
        if (rlen != tlen) memmove(lines[row] + col + rlen, lines[row] + col + tlen, (size_t)tail_len);
        memcpy(lines[row] + col, repl, (size_t)rlen);
        line_len[row] = newlen;
        lines[row][line_len[row]] = 0;

        count++;
        col += rlen;
    }
    return count;
}

static void do_replace_all(void)
{
    sel_active = 0;
    char term[64], repl[64];
    prompt_filename("Replace - find: ", term, sizeof(term));
    if (!term[0]) { redraw(); return; }
    prompt_filename("Replace - with: ", repl, sizeof(repl));

    int total = 0;
    for (int r = 0; r < num_lines; r++) total += replace_in_line(r, term, repl);
    if (total > 0) modified = 1;

    char msg[40];
    int k = 0;
    const char *p = "Replaced ";
    while (*p) msg[k++] = *p++;
    char numbuf[12];
    int n = 0, v = total;
    if (v == 0) numbuf[n++] = '0';
    else { char tmp[12]; int tn = 0; while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; } while (tn > 0) numbuf[n++] = tmp[--tn]; }
    for (int i = 0; i < n; i++) msg[k++] = numbuf[i];
    p = " occurrence(s).";
    while (*p) msg[k++] = *p++;
    msg[k] = 0;

    redraw();
    status_line(msg);
}

/* ── editing primitives ─────────────────────────────────────────────────── */

static void insert_char(char c)
{
    if (line_len[cur_row] >= CONTENT_W) return;
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
        if (merged_len <= CONTENT_W) {
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

/* ── go-to-line ─────────────────────────────────────────────────────────── */

static void do_goto_line(void)
{
    char buf[12];
    prompt_filename("Go to line: ", buf, sizeof(buf));
    if (!buf[0]) { redraw(); return; }
    int n = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) n = n * 10 + (buf[i] - '0');
    if (n < 1) n = 1;
    if (n > num_lines) n = num_lines;
    cur_row = n - 1;
    if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
    redraw();
}

/* ── main loop ──────────────────────────────────────────────────────────── */

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
                        if (i == 0) { do_new(); redraw(); }
                        else if (i == 1) { do_open(); redraw(); }
                        else if (i == 2) { do_save(); redraw(); }
                        else if (i == 3) do_find();
                        else if (i == 4) do_replace_all();
                        break;
                    }
                }
            } else if (ccx == SCROLLBAR_X && ccy >= 0 && ccy < NP_ROWS && num_lines > NP_ROWS) {
                int max_top = num_lines - NP_ROWS;
                int target = (NP_ROWS > 1) ? (ccy * max_top) / (NP_ROWS - 1) : 0;
                if (target < 0) target = 0;
                if (target > max_top) target = max_top;
                cur_row = target;
                if (cur_row >= num_lines) cur_row = num_lines - 1;
                if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                sel_active = 0;
                redraw();
            } else if (ccx >= LINE_NUM_W && ccy >= 0 && ccy < NP_ROWS) {
                /* click in content area */
                int clicked_row = view_top + ccy;
                int clicked_col = ccx - LINE_NUM_W;
                if (clicked_row < num_lines) {
                    cur_row = clicked_row;
                    if (clicked_col > line_len[cur_row]) clicked_col = line_len[cur_row];
                    cur_col = clicked_col;
                    sel_active = 0;
                    redraw();
                }
            }
            continue;
        }

        int wheel = wm_get_content_wheel();
        if (wheel) {
            int max_top = num_lines - NP_ROWS;
            if (max_top < 0) max_top = 0;
            view_top -= wheel * 3;
            if (view_top < 0) view_top = 0;
            if (view_top > max_top) view_top = max_top;

            if (cur_row < view_top) cur_row = view_top;
            if (cur_row > view_top + NP_ROWS - 1) cur_row = view_top + NP_ROWS - 1;
            if (cur_row >= num_lines) cur_row = num_lines - 1;
            if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
            redraw();
            continue;
        }

        int spec = keyboard_get_special();
        if (spec) {
            int shift = keyboard_shift_held();
            if (shift && !sel_active) {
                sel_anchor_row = cur_row;
                sel_anchor_col = cur_col;
                sel_active = 1;
            } else if (!shift) {
                sel_active = 0;
            }

            if (spec == 1) {
                if (cur_col > 0) cur_col--;
                else if (cur_row > 0) { cur_row--; cur_col = line_len[cur_row]; }
            } else if (spec == 2) {
                if (cur_col < line_len[cur_row]) cur_col++;
                else if (cur_row < num_lines - 1) { cur_row++; cur_col = 0; }
            } else if (spec == 3) {
                if (cur_row > 0) { cur_row--; if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row]; }
            } else if (spec == 4) {
                if (cur_row < num_lines - 1) { cur_row++; if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row]; }
            }
            redraw();
            continue;
        }

        if (!keyboard_data_available() || !wm_current_task_has_focus()) {
            task_yield();
            continue;
        }

        char c = keyboard_getchar();

        if (c == 0x13) {          /* Ctrl+S */
            do_save();
            redraw();
        } else if (c == 0x0F) {  /* Ctrl+O */
            do_open();
            redraw();
        } else if (c == 0x0E) {  /* Ctrl+N */
            do_new();
            redraw();
        } else if (c == 0x06) {  /* Ctrl+F */
            do_find();
        } else if (c == 0x12) {  /* Ctrl+R */
            do_replace_all();
        } else if (c == 0x07) {  /* Ctrl+G */
            do_goto_line();
        } else if (c == 0x03) {  /* Ctrl+C */
            copy_selection();
        } else if (c == 0x18) {  /* Ctrl+X */
            cut_selection();
            redraw();
        } else if (c == 0x16) {  /* Ctrl+V */
            paste_clipboard();
            redraw();
        } else if (c == '\n') {
            if (!selection_empty()) delete_selection();
            insert_newline();
            redraw();
        } else if (c == '\b' || c == 127) {
            if (!selection_empty()) delete_selection();
            else backspace();
            redraw();
        } else if ((unsigned char)c >= ' ' && c < 127) {
            if (!selection_empty()) delete_selection();
            insert_char(c);
            redraw();
        }
    }
}
