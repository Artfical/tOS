#include "pdfview.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"
#include "pdf_parse.h"

#define PV_COLS 79
#define PV_ROWS 22
#define TOOLBAR_ROW 0
#define CANVAS_Y0 1
#define STATUS_ROW (PV_ROWS - 1)
#define CANVAS_H (STATUS_ROW - CANVAS_Y0)
#define CANVAS_W PV_COLS

#define NUM_BUTTONS 7
static const char *btn_labels[NUM_BUTTONS] = { "Open", "Zoom In", "Zoom Out", "Fit", "Prev", "Next", "Find" };
static int btn_x0[NUM_BUTTONS], btn_x1[NUM_BUTTONS];

static pdf_doc_t *doc;
static int cur_page;
static double scale;      /* points per screen cell */
static double view_x, view_y; /* pan offset, in points, from the page's top-left */
static char filename[64];
static char status_msg[PV_COLS + 1];
static char pending_path[64];
static int has_pending;

static char last_search[64];
static int search_match_page = -1;
static int search_match_run = -1;

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
    while (s[i] && i < PV_COLS) { status_msg[i] = s[i]; i++; }
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

static void free_doc(void)
{
    if (doc) { pdf_free(doc); free(doc); doc = NULL; }
}

static pdf_page_t *page(void)
{
    if (!doc || cur_page < 0 || cur_page >= doc->page_count) return NULL;
    return &doc->pages[cur_page];
}

static void clamp_view(void)
{
    pdf_page_t *pg = page();
    if (!pg) return;
    if (scale < 0.1) scale = 0.1;
    double pw = pg->mbx1 - pg->mbx0;
    double ph = pg->mby1 - pg->mby0;
    double max_vx = pw - CANVAS_W * scale;
    double max_vy = ph - CANVAS_H * scale;
    if (max_vx < 0) max_vx = 0;
    if (max_vy < 0) max_vy = 0;
    if (view_x > max_vx) view_x = max_vx;
    if (view_y > max_vy) view_y = max_vy;
    if (view_x < 0) view_x = 0;
    if (view_y < 0) view_y = 0;
}

static void fit_view(void)
{
    pdf_page_t *pg = page();
    if (!pg) { scale = 1; view_x = view_y = 0; return; }
    double pw = pg->mbx1 - pg->mbx0;
    if (pw < 1) pw = 612;
    scale = pw / (double)CANVAS_W;
    if (scale < 0.1) scale = 0.1;
    view_x = 0;
    view_y = 0;
}

void pdfview_open_path(const char *path)
{
    strncpy(pending_path, path, sizeof(pending_path) - 1);
    pending_path[sizeof(pending_path) - 1] = 0;
    has_pending = 1;
}

static void do_open(const char *path)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) { set_status("Open: file not found."); return; }
    uint32_t sz = fsbridge_size(path);
    uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
    if (!buf) { set_status("Open: out of memory."); return; }
    fsbridge_read(path, buf, sz, 0);

    pdf_doc_t *new_doc = (pdf_doc_t *)malloc(sizeof(pdf_doc_t));
    if (!new_doc) { free(buf); set_status("Open: out of memory."); return; }

    char err[80];
    int rc = pdf_parse(buf, sz, new_doc, err, sizeof(err));
    free(buf);

    if (rc != 0) { free(new_doc); set_status(err); return; }

    free_doc();
    doc = new_doc;
    cur_page = 0;
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
    uint8_t bg_color = pcolor(VGA_DARK_GREY, VGA_BLACK);
    for (int cy = 0; cy < CANVAS_H; cy++) {
        terminal_setpos(0, (size_t)(CANVAS_Y0 + cy));
        terminal_setcolor(bg_color);
        for (int cx = 0; cx < CANVAS_W; cx++) terminal_putchar(' ');
    }

    pdf_page_t *pg = page();
    if (!pg) return;

    uint8_t text_color = pcolor(VGA_WHITE, VGA_BLACK);
    uint8_t match_color = pcolor(VGA_BLACK, VGA_LIGHT_BROWN);
    double pw = pg->mbx1 - pg->mbx0;
    double ph = pg->mby1 - pg->mby0;

    for (int i = 0; i < pg->run_count; i++) {
        pdf_run_t *r = &pg->runs[i];
        double px = r->x - pg->mbx0;
        double py_top = ph - (r->y - pg->mby0); /* flip: PDF origin is bottom-left */
        if (px < 0 || px > pw || py_top < 0 || py_top > ph) continue;

        double col0 = (px - view_x) / scale;
        double row0 = (py_top - view_y) / scale;
        if (row0 < -1 || row0 >= CANVAS_H + 1) continue;
        int row = (int)row0;
        if (row < 0 || row >= CANVAS_H) continue;

        uint8_t color = (cur_page == search_match_page && i == search_match_run) ? match_color : text_color;
        for (int c = 0; c < r->len; c++) {
            int col = (int)(col0 + c);
            if (col < 0 || col >= CANVAS_W) continue;
            char ch = r->text[c];
            if (ch < 32 || ch > 126) ch = '.';
            terminal_setcolor(color);
            terminal_setpos((size_t)col, (size_t)(CANVAS_Y0 + row));
            terminal_putchar(ch);
        }
    }
}

static void draw_status(void)
{
    char line[PV_COLS + 1];
    int k = 0;
    if (doc) {
        const char *name = filename[0] ? filename : "Untitled";
        while (name[k] && k < 24) { line[k] = name[k]; k++; }
        line[k++] = ' '; line[k++] = ' ';
        line[k++] = 'P'; line[k++] = 'a'; line[k++] = 'g'; line[k++] = 'e'; line[k++] = ' ';
        k += fmt_uint(line + k, (uint32_t)(cur_page + 1));
        line[k++] = '/';
        k += fmt_uint(line + k, (uint32_t)doc->page_count);
        line[k++] = ' '; line[k++] = ' ';
        line[k++] = 'Z'; line[k++] = 'o'; line[k++] = 'o'; line[k++] = 'm'; line[k++] = ':'; line[k++] = ' ';
        uint32_t pct = (uint32_t)((100.0 * 1.0) / scale + 0.5);
        k += fmt_uint(line + k, pct);
        line[k++] = '%';
        const char *hint = "  [/] page  arrows pan  Ctrl+F find";
        const char *p = hint;
        while (*p && k < PV_COLS) line[k++] = *p++;
    } else {
        const char *s = status_msg[0] ? status_msg : "No PDF open. Click Open to load a .pdf file.";
        while (s[k] && k < PV_COLS) { line[k] = s[k]; k++; }
    }
    while (k < PV_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_ROW, line, pcolor(VGA_BLACK, VGA_LIGHT_GREY));
}

static void redraw(void)
{
    draw_toolbar();
    draw_canvas();
    draw_status();
}

static void goto_page(int delta)
{
    if (!doc) return;
    int np = cur_page + delta;
    if (np < 0) np = 0;
    if (np >= doc->page_count) np = doc->page_count - 1;
    if (np != cur_page) { cur_page = np; view_x = view_y = 0; }
}

static int istr_contains(const char *hay, const char *needle)
{
    int hlen = (int)strlen(hay), nlen = (int)strlen(needle);
    if (nlen == 0) return 0;
    for (int i = 0; i + nlen <= hlen; i++) {
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* Jumps to the next run (across pages, wrapping around the whole
 * document) whose extracted text contains the search term, picking up
 * right after the previous match so repeated calls step through every
 * occurrence instead of re-finding the first one. */
static void do_search(int prompt_new)
{
    if (!doc) { set_status("No PDF open."); return; }
    if (prompt_new || !last_search[0]) {
        char term[64];
        prompt_filename("Find: ", term, sizeof(term));
        if (term[0]) {
            strncpy(last_search, term, sizeof(last_search) - 1);
            last_search[sizeof(last_search) - 1] = 0;
            search_match_page = -1;
            search_match_run = -1;
        }
    }
    if (!last_search[0]) { redraw(); return; }

    int start_page = cur_page;
    int start_run = (search_match_page == cur_page) ? search_match_run + 1 : 0;

    for (int offset = 0; offset < doc->page_count; offset++) {
        int p = (start_page + offset) % doc->page_count;
        int r0 = (offset == 0) ? start_run : 0;
        pdf_page_t *pg = &doc->pages[p];
        for (int r = r0; r < pg->run_count; r++) {
            if (!istr_contains(pg->runs[r].text, last_search)) continue;

            cur_page = p;
            search_match_page = p;
            search_match_run = r;

            double ph = pg->mby1 - pg->mby0;
            double px = pg->runs[r].x - pg->mbx0;
            double py_top = ph - (pg->runs[r].y - pg->mby0);
            view_x = px - (CANVAS_W * scale) * 0.1;
            view_y = py_top - (CANVAS_H * scale) * 0.3;
            if (view_x < 0) view_x = 0;
            if (view_y < 0) view_y = 0;
            clamp_view();
            set_status("Found. Ctrl+F again for next.");
            redraw();
            return;
        }
    }
    search_match_page = -1;
    search_match_run = -1;
    set_status("Not found.");
    redraw();
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
                if (doc) { scale *= 0.8; clamp_view(); }
            } else if (i == 2) {
                if (doc) { scale *= 1.25; clamp_view(); }
            } else if (i == 3) {
                if (doc) fit_view();
            } else if (i == 4) {
                goto_page(-1);
            } else if (i == 5) {
                goto_page(1);
            } else if (i == 6) {
                do_search(1);
            }
            return;
        }
    }
}

void pdfview_run(void)
{
    free_doc();
    cur_page = 0;
    scale = 1;
    view_x = view_y = 0;
    filename[0] = 0;
    status_msg[0] = 0;
    last_search[0] = 0;
    search_match_page = -1;
    search_match_run = -1;

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
        if (spec && doc) {
            double step = scale * 3;
            if (spec == 1) view_x -= step;
            else if (spec == 2) view_x += step;
            else if (spec == 3) view_y -= step;
            else if (spec == 4) view_y += step;
            clamp_view();
            redraw();
            task_yield();
            continue;
        }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == '[') { goto_page(-1); redraw(); }
            else if (c == ']') { goto_page(1); redraw(); }
            else if (c == '+' || c == '=') { if (doc) { scale *= 0.8; clamp_view(); redraw(); } }
            else if (c == '-' || c == '_') { if (doc) { scale *= 1.25; clamp_view(); redraw(); } }
            else if (c == 0x06) { do_search(1); } /* Ctrl+F */
            else if (c == 'n' || c == 'N') { do_search(0); }
        }

        task_yield();
    }
}
