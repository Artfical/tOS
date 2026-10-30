/* An xterm-style terminal on a character surface (terminal.h): what the terminal programs that
 * draw whole screens expect (cursor movement, erasing, colours, the scroll region, the alternate
 * screen, queries it answers) and the plain output the shell and old programs have always sent.
 *
 * Colours are the 16 of the surface; 256-colour and true-colour requests become the nearest of
 * them. Characters are bytes of the CP437 font tOS draws: UTF-8 (when a Linux program writes it)
 * is mapped to the nearest glyph (vt_map.h), box drawing included.
 *
 * Only terminal.h's types are used here, so the file builds on its own for tests. */
#include "terminal.h"
#include "vt_map.h"

#define W TERM_SURFACE_W
#define H TERM_SURFACE_H

enum { ST_GROUND, ST_ESC, ST_ESC_INTER, ST_CSI, ST_OSC, ST_OSC_ESC, ST_STR, ST_STR_ESC, ST_CHARSET };

/* the colours wm_gfx.c draws the 16 attributes with, to find the nearest of a requested one */
static const uint32_t vt_palette[16] = {
    0x1A1B21, 0x3B82F6, 0x22C55E, 0x06B6D4, 0xEF4444, 0xD946EF, 0xF59E0B, 0xD4D4D8,
    0x71717A, 0x60A5FA, 0x4ADE80, 0x22D3EE, 0xF87171, 0xF0ABFC, 0xFDE047, 0xFAFAFA,
};
/* ANSI colour n (0-7: black red green yellow blue magenta cyan white) as an attribute index (VGA order) */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint16_t mk(uint8_t ch, uint8_t attr) { return (uint16_t)ch | (uint16_t)attr << 8; }

static void memfill16(uint16_t *p, uint16_t v, int n) { for (int i = 0; i < n; i++) p[i] = v; }
static void memcopy16(uint16_t *d, const uint16_t *s, int n)
{
    if (d < s) for (int i = 0; i < n; i++) d[i] = s[i];
    else for (int i = n - 1; i >= 0; i--) d[i] = s[i];
}

static uint8_t attr_of(const term_surface_t *s)
{
    uint8_t fg = s->fg, bg = s->bg;
    if ((s->sgr & 1) && fg < 8) fg += 8;
    if (s->sgr & 2) { uint8_t t = fg; fg = bg; bg = t; }
    return (uint8_t)(fg | bg << 4);
}

static void sync_color(term_surface_t *s) { s->color = attr_of(s); }

void vt_set_color(term_surface_t *s, uint8_t attr)
{
    s->fg = attr & 15;
    s->bg = attr >> 4;
    s->sgr = 0;
    s->color = attr;
}

static void defaults(term_surface_t *s)
{
    s->fg = 7; s->bg = 0; s->sgr = 0;
    sync_color(s);
    s->top = 0; s->bottom = H - 1;
    s->wrap_pending = 0; s->autowrap = 1; s->cursor_visible = 1;
    s->app_cursor = 0; s->bracketed_paste = 0; s->mouse_mode = 0; s->mouse_sgr = 0;
    s->insert_mode = 0; s->origin = 0; s->vt_gfx = 0;
}

void vt_init(term_surface_t *s)
{
    s->vt_state = ST_GROUND; s->vt_nparams = 0; s->vt_prefix = 0; s->vt_inter = 0; s->vt_utf8_left = 0;
    s->vt_osc_len = 0; s->vt_last = 0;
    s->saved_row = s->saved_col = 0;
    s->saved_fg = 7; s->saved_bg = 0; s->saved_sgr = 0; s->saved_gfx = 0;
    s->alt_screen = 0; s->utf8 = 0; s->xterm_keys = 0;
    s->title[0] = 0;
    s->reply_len = 0;
    defaults(s);
}

/* the program is gone (or died in the middle of a screen): the shell gets a usable terminal back */
void vt_reset_modes(term_surface_t *s)
{
    if (s->alt_screen) {
        memcopy16(s->cells, s->alt_cells, W * H);
        s->cur_row = s->alt_saved_row; s->cur_col = s->alt_saved_col;
        s->alt_screen = 0;
    }
    defaults(s);
    s->utf8 = 0; s->xterm_keys = 0;
    s->vt_state = ST_GROUND; s->vt_utf8_left = 0;
    s->tty.c_iflag = 0x0502; s->tty.c_oflag = 0x0005; s->tty.c_lflag = 0x8A3B;     /* cooked again */
    s->tty.c_cc[6] = 1; s->tty.c_cc[5] = 0;
    s->tty.inq_head = s->tty.inq_tail = 0; s->tty.line_len = 0; s->tty.line_done = 0; s->tty.eof_pending = 0;
    if (s->cur_col >= W) s->cur_col = W - 1;
}

/* ---------------------------------------------------------------- screen operations */

static void scroll_up(term_surface_t *s, int n)       /* lines move up inside the region; blanks come in below */
{
    int top = s->top, bot = s->bottom;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        if (top == 0 && bot == H - 1 && !s->alt_screen) {
            /* the line leaving the top goes to the scrollback (terminal.c's, the surface's own history) */
            for (int x = 0; x < W; x++) s->scrollback[s->scrollback_head][x] = s->cells[x];
            s->scrollback_head = (s->scrollback_head + 1) % TERM_SCROLLBACK_LINES;
            if (s->scrollback_count < TERM_SCROLLBACK_LINES) s->scrollback_count++;
        }
        memcopy16(&s->cells[top * W], &s->cells[(top + 1) * W], (bot - top) * W);
        memfill16(&s->cells[bot * W], mk(' ', s->color), W);
    }
}

static void scroll_down(term_surface_t *s, int n)
{
    int top = s->top, bot = s->bottom;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        memcopy16(&s->cells[(top + 1) * W], &s->cells[top * W], (bot - top) * W);
        memfill16(&s->cells[top * W], mk(' ', s->color), W);
    }
}

static void linefeed(term_surface_t *s)
{
    if (s->cur_row == s->bottom) scroll_up(s, 1);
    else if (s->cur_row < H - 1) s->cur_row++;
}

static void reverse_index(term_surface_t *s)
{
    if (s->cur_row == s->top) scroll_down(s, 1);
    else if (s->cur_row > 0) s->cur_row--;
}

static void put_cell(term_surface_t *s, uint8_t ch)
{
    if (s->wrap_pending) {
        s->wrap_pending = 0;
        if (s->autowrap) { s->cur_col = 0; linefeed(s); }
    }
    if (s->insert_mode && s->cur_col < W - 1)
        memcopy16(&s->cells[s->cur_row * W + s->cur_col + 1], &s->cells[s->cur_row * W + s->cur_col], W - 1 - s->cur_col);
    s->cells[s->cur_row * W + s->cur_col] = mk(ch, s->color);
    if (s->cur_col == W - 1) s->wrap_pending = 1;
    else s->cur_col++;
}

static void clamp(term_surface_t *s)
{
    if (s->cur_row < 0) s->cur_row = 0;
    if (s->cur_row >= H) s->cur_row = H - 1;
    if (s->cur_col < 0) s->cur_col = 0;
    if (s->cur_col >= W) s->cur_col = W - 1;
}

static void erase_cells(term_surface_t *s, int row, int from, int to)   /* [from, to) of a row */
{
    if (from < 0) from = 0;
    if (to > W) to = W;
    if (from < to) memfill16(&s->cells[row * W + from], mk(' ', s->color), to - from);
}

static void goto_rc(term_surface_t *s, int row, int col)
{
    s->wrap_pending = 0;
    if (s->origin) { row += s->top; if (row > s->bottom) row = s->bottom; }
    s->cur_row = row; s->cur_col = col;
    clamp(s);
}

/* ---------------------------------------------------------------- characters */

static const uint8_t gfx_map[32] = {     /* DEC special graphics, 0x60..0x7F -> CP437 */
    0x04, 0xB1, 0x09, 0x0C, 0x0D, 0x0A, 0xF8, 0xF1, 0x0F, 0x0B, 0xD9, 0xBF, 0xDA, 0xC0, 0xC5, 0x2D,
    0x2D, 0xC4, 0x2D, 0x5F, 0xC3, 0xB4, 0xC1, 0xC2, 0xB3, 0xF3, 0xF2, 0xE3, 0x3D, 0x9C, 0xFA, 0x20
};

static uint8_t map_unicode(uint32_t cp)
{
    if (cp < 0x80) return (uint8_t)cp;
    int lo = 0, hi = (int)VT_MAP_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (vt_map[mid].cp == cp) return vt_map[mid].b;
        if (vt_map[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    /* zero-width and combining marks take no cell */
    if ((cp >= 0x0300 && cp <= 0x036F) || cp == 0x200B || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    return '?';
}

static void print_cp(term_surface_t *s, uint32_t cp)
{
    uint8_t ch;
    if (s->vt_gfx && cp >= 0x60 && cp < 0x80) ch = gfx_map[cp - 0x60];
    else ch = map_unicode(cp);
    if (!ch && cp >= 0x80) return;
    s->vt_last = cp;
    put_cell(s, ch);
}

/* ---------------------------------------------------------------- sequences */

static int nearest16(int r, int g, int b)
{
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)(vt_palette[i] >> 16 & 255), dg = g - (int)(vt_palette[i] >> 8 & 255), db = b - (int)(vt_palette[i] & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int color256(int n)
{
    if (n < 8) return ansi_to_vga[n];
    if (n < 16) return ansi_to_vga[n - 8] + 8;
    if (n < 232) {
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return nearest16(lv[n / 36], lv[n / 6 % 6], lv[n % 6]);
    }
    int g = 8 + (n - 232) * 10;
    return nearest16(g, g, g);
}

static int param(const term_surface_t *s, int i, int def)
{
    return (i < s->vt_nparams && s->vt_param[i] > 0) ? s->vt_param[i] : def;
}

static void sgr(term_surface_t *s)
{
    int n = s->vt_nparams ? s->vt_nparams : 1;
    for (int i = 0; i < n; i++) {
        int p = i < s->vt_nparams ? s->vt_param[i] : 0;
        if (p < 0) p = 0;
        if (p == 0) { s->fg = 7; s->bg = 0; s->sgr = 0; }
        else if (p == 1) s->sgr |= 1;
        else if (p == 22) s->sgr &= (uint8_t)~1;
        else if (p == 7) s->sgr |= 2;
        else if (p == 27) s->sgr &= (uint8_t)~2;
        else if (p >= 30 && p <= 37) s->fg = ansi_to_vga[p - 30];
        else if (p == 39) s->fg = 7;
        else if (p >= 40 && p <= 47) s->bg = ansi_to_vga[p - 40];
        else if (p == 49) s->bg = 0;
        else if (p >= 90 && p <= 97) s->fg = (uint8_t)(ansi_to_vga[p - 90] + 8);
        else if (p >= 100 && p <= 107) s->bg = (uint8_t)(ansi_to_vga[p - 100] + 8);
        else if (p == 38 || p == 48) {
            int c = -1;
            if (i + 1 < n && s->vt_param[i + 1] == 5 && i + 2 < n) { c = color256(s->vt_param[i + 2]); i += 2; }
            else if (i + 1 < n && s->vt_param[i + 1] == 2 && i + 4 < n) {
                c = nearest16(s->vt_param[i + 2], s->vt_param[i + 3], s->vt_param[i + 4]); i += 4;
            } else i = n;
            if (c >= 0) { if (p == 38) s->fg = (uint8_t)c; else s->bg = (uint8_t)c; }
        }
    }
    sync_color(s);
}

static void reply(term_surface_t *s, const char *t)
{
    for (; *t && s->reply_len < sizeof(s->reply); t++) s->reply[s->reply_len++] = (uint8_t)*t;
}

static void reply_num(term_surface_t *s, int n)
{
    char b[8]; int k = 0;
    if (!n) b[k++] = '0';
    while (n && k < 7) { b[k++] = (char)('0' + n % 10); n /= 10; }
    while (k && s->reply_len < sizeof(s->reply)) s->reply[s->reply_len++] = (uint8_t)b[--k];
}

static void set_alt(term_surface_t *s, int on, int save_cursor)
{
    if (on == s->alt_screen) return;
    if (on) {
        s->alt_saved_row = s->cur_row; s->alt_saved_col = s->cur_col;
        uint16_t tmp[W];                       /* the main screen goes into alt_cells, a blank one comes */
        for (int y = 0; y < H; y++) {
            memcopy16(tmp, &s->cells[y * W], W);
            memfill16(&s->cells[y * W], mk(' ', s->color), W);
            memcopy16(&s->alt_cells[y * W], tmp, W);
        }
        s->alt_screen = 1;
        s->cur_row = 0; s->cur_col = 0;
    } else {
        memcopy16(s->cells, s->alt_cells, W * H);
        s->alt_screen = 0;
        if (save_cursor) { s->cur_row = s->alt_saved_row; s->cur_col = s->alt_saved_col; }
        clamp(s);
    }
    s->wrap_pending = 0;
}

static void set_mode(term_surface_t *s, int priv, int mode, int on)
{
    if (!priv) {
        if (mode == 4) s->insert_mode = (uint8_t)on;
        return;
    }
    switch (mode) {
        case 1: s->app_cursor = (uint8_t)on; break;
        case 6: s->origin = (uint8_t)on; goto_rc(s, 0, 0); break;
        case 7: s->autowrap = (uint8_t)on; break;
        case 25: s->cursor_visible = (uint8_t)on; break;
        case 47: case 1047: set_alt(s, on, 0); break;
        case 1049: set_alt(s, on, 1); break;
        case 1000: case 1002: case 1003: s->mouse_mode = on ? (uint8_t)(mode == 1000 ? 1 : mode == 1002 ? 2 : 3) : 0; break;
        case 1006: s->mouse_sgr = (uint8_t)on; break;
        case 2004: s->bracketed_paste = (uint8_t)on; break;
        default: break;                        /* 12 (blink), 1004 (focus), 2026 (sync) and the rest: accepted */
    }
}

static void csi(term_surface_t *s, uint8_t f)
{
    int n1 = param(s, 0, 1);
    s->wrap_pending = (f == 'm' || f == 'h' || f == 'l') ? s->wrap_pending : 0;
    switch (f) {
        case 'A': { int lim = s->cur_row >= s->top ? s->top : 0; s->cur_row -= n1; if (s->cur_row < lim) s->cur_row = lim; break; }
        case 'B': { int lim = s->cur_row <= s->bottom ? s->bottom : H - 1; s->cur_row += n1; if (s->cur_row > lim) s->cur_row = lim; break; }
        case 'C': case 'a': s->cur_col += n1; break;
        case 'D': s->cur_col -= n1; break;
        case 'E': s->cur_row += n1; s->cur_col = 0; break;
        case 'F': s->cur_row -= n1; s->cur_col = 0; break;
        case 'G': case '`': s->cur_col = n1 - 1; break;
        case 'd': goto_rc(s, n1 - 1, s->cur_col); break;
        case 'H': case 'f': goto_rc(s, n1 - 1, param(s, 1, 1) - 1); break;
        case 'J': {
            int m = s->vt_nparams ? s->vt_param[0] : 0;
            if (m == 0) { erase_cells(s, s->cur_row, s->cur_col, W); for (int y = s->cur_row + 1; y < H; y++) erase_cells(s, y, 0, W); }
            else if (m == 1) { for (int y = 0; y < s->cur_row; y++) erase_cells(s, y, 0, W); erase_cells(s, s->cur_row, 0, s->cur_col + 1); }
            else if (m == 2) { for (int y = 0; y < H; y++) erase_cells(s, y, 0, W); }
            else if (m == 3) { s->scrollback_count = 0; s->scrollback_head = 0; s->view_offset = 0; }
            break;
        }
        case 'K': {
            int m = s->vt_nparams ? s->vt_param[0] : 0;
            if (m == 0) erase_cells(s, s->cur_row, s->cur_col, W);
            else if (m == 1) erase_cells(s, s->cur_row, 0, s->cur_col + 1);
            else if (m == 2) erase_cells(s, s->cur_row, 0, W);
            break;
        }
        case 'L': case 'M':
            if (s->cur_row >= s->top && s->cur_row <= s->bottom) {
                int save = s->top; s->top = s->cur_row;
                if (f == 'L') scroll_down(s, n1); else scroll_up(s, n1);
                s->top = save;
                s->cur_col = 0;
            }
            break;
        case '@': {
            int n = n1 > W - s->cur_col ? W - s->cur_col : n1;
            memcopy16(&s->cells[s->cur_row * W + s->cur_col + n], &s->cells[s->cur_row * W + s->cur_col], W - s->cur_col - n);
            erase_cells(s, s->cur_row, s->cur_col, s->cur_col + n);
            break;
        }
        case 'P': {
            int n = n1 > W - s->cur_col ? W - s->cur_col : n1;
            memcopy16(&s->cells[s->cur_row * W + s->cur_col], &s->cells[s->cur_row * W + s->cur_col + n], W - s->cur_col - n);
            erase_cells(s, s->cur_row, W - n, W);
            break;
        }
        case 'X': erase_cells(s, s->cur_row, s->cur_col, s->cur_col + n1); break;
        case 'S': scroll_up(s, n1); break;
        case 'T': scroll_down(s, n1); break;
        case 'b': for (int i = 0; i < n1 && i < W * H && s->vt_last; i++) print_cp(s, s->vt_last); break;
        case 'r': {
            int t = param(s, 0, 1), b = param(s, 1, H);
            if (b > H) b = H;
            if (t < b) { s->top = t - 1; s->bottom = b - 1; goto_rc(s, 0, 0); }
            break;
        }
        case 's': s->saved_row = s->cur_row; s->saved_col = s->cur_col; s->saved_fg = s->fg; s->saved_bg = s->bg; s->saved_sgr = s->sgr; break;
        case 'u': s->cur_row = s->saved_row; s->cur_col = s->saved_col; s->fg = s->saved_fg; s->bg = s->saved_bg; s->sgr = s->saved_sgr; sync_color(s); break;
        case 'm': if (!s->vt_prefix) sgr(s); break;
        case 'h': case 'l':
            for (int i = 0; i < (s->vt_nparams ? s->vt_nparams : 1); i++)
                set_mode(s, s->vt_prefix == '?', i < s->vt_nparams ? s->vt_param[i] : 0, f == 'h');
            break;
        case 'n':
            if (s->vt_prefix == 0 && param(s, 0, 0) == 5) reply(s, "\033[0n");
            else if (s->vt_prefix == 0 && param(s, 0, 0) == 6) {
                reply(s, "\033["); reply_num(s, s->cur_row + 1 - (s->origin ? s->top : 0)); reply(s, ";"); reply_num(s, s->cur_col + 1); reply(s, "R");
            }
            break;
        case 'c':
            if (s->vt_prefix == 0) reply(s, "\033[?1;2c");              /* VT100 with advanced video */
            break;
        default: break;                        /* tab stops, cursor style, window ops, kitty keyboard queries: ignored */
    }
    clamp(s);
}

static void esc_final(term_surface_t *s, uint8_t c)
{
    switch (c) {
        case '7': s->saved_row = s->cur_row; s->saved_col = s->cur_col; s->saved_fg = s->fg; s->saved_bg = s->bg; s->saved_sgr = s->sgr; s->saved_gfx = s->vt_gfx; break;
        case '8': s->cur_row = s->saved_row; s->cur_col = s->saved_col; s->fg = s->saved_fg; s->bg = s->saved_bg; s->sgr = s->saved_sgr; s->vt_gfx = s->saved_gfx; sync_color(s); s->wrap_pending = 0; clamp(s); break;
        case 'D': linefeed(s); break;
        case 'E': s->cur_col = 0; linefeed(s); break;
        case 'M': reverse_index(s); break;
        case 'c': {
            for (int y = 0; y < H; y++) erase_cells(s, y, 0, W);
            s->cur_row = s->cur_col = 0;
            if (s->alt_screen) set_alt(s, 0, 0);
            defaults(s);
            break;
        }
        default: break;                        /* = > (keypad) and the rest */
    }
    s->vt_state = ST_GROUND;
}

/* ---------------------------------------------------------------- the byte stream */

static void exec_ctl(term_surface_t *s, uint8_t c)
{
    switch (c) {
        case '\n': case 0x0B: case 0x0C:
            if ((s->tty.c_oflag & 4) && (s->tty.c_oflag & 1)) s->cur_col = 0;       /* OPOST and ONLCR */
            s->wrap_pending = 0;
            linefeed(s);
            break;
        case '\r': s->cur_col = 0; s->wrap_pending = 0; break;
        case '\b': if (s->cur_col > 0) s->cur_col--; s->wrap_pending = 0; break;
        case '\t': { int nc = (s->cur_col + 8) & ~7; s->cur_col = nc >= W ? W - 1 : nc; s->wrap_pending = 0; break; }
        default: break;                        /* bell, shift in/out */
    }
}

void vt_putbyte(term_surface_t *s, uint8_t c)
{
    s->view_offset = 0;
    switch (s->vt_state) {
        case ST_ESC:
            if (c == '[') { s->vt_state = ST_CSI; s->vt_nparams = 0; s->vt_prefix = 0; s->vt_inter = 0; for (int i = 0; i < 16; i++) s->vt_param[i] = -1; return; }
            if (c == ']') { s->vt_state = ST_OSC; s->vt_osc_len = 0; return; }
            if (c == 'P' || c == 'X' || c == '^' || c == '_') { s->vt_state = ST_STR; return; }
            if (c == '(' || c == ')' || c == '*' || c == '+') { s->vt_state = ST_CHARSET; s->vt_inter = c; return; }
            if (c == '#' || c == ' ' || c == '%') { s->vt_state = ST_ESC_INTER; return; }
            if (c == 0x1B) return;
            esc_final(s, c);
            return;
        case ST_ESC_INTER: s->vt_state = ST_GROUND; return;
        case ST_CHARSET:
            if (s->vt_inter == '(') s->vt_gfx = (uint8_t)(c == '0');
            s->vt_state = ST_GROUND;
            return;
        case ST_CSI:
            if (c >= '0' && c <= '9') {
                if (s->vt_nparams == 0) s->vt_nparams = 1;
                int *p = &s->vt_param[s->vt_nparams - 1];
                *p = (*p < 0 ? 0 : *p) * 10 + (c - '0');
                if (*p > 99999) *p = 99999;
                return;
            }
            if (c == ';' || c == ':') {
                if (s->vt_nparams == 0) s->vt_nparams = 1;
                if (s->vt_nparams < 16) s->vt_nparams++;
                return;
            }
            if (c >= '<' && c <= '?') { s->vt_prefix = c; return; }
            if (c >= 0x20 && c <= 0x2F) { s->vt_inter = c; return; }
            if (c == 0x1B) { s->vt_state = ST_ESC; return; }
            if (c < 0x20 || c == 0x7F) { exec_ctl(s, c); return; }              /* controls run inside a sequence */
            for (int i = 0; i < s->vt_nparams; i++) if (s->vt_param[i] < 0) s->vt_param[i] = 0;
            s->vt_state = ST_GROUND;
            csi(s, c);
            return;
        case ST_OSC:
            if (c == 7) {
                s->vt_state = ST_GROUND;
                return;
            }
            if (c == 0x1B) { s->vt_state = ST_OSC_ESC; return; }
            /* "0;title" and "2;title" set the window title; the rest (colour queries...) is dropped */
            if (s->vt_osc_len == 0) s->vt_inter = c;
            else if (s->vt_osc_len >= 2 && (s->vt_inter == '0' || s->vt_inter == '2') && s->vt_osc_len - 2 < (int)sizeof(s->title) - 1) {
                s->title[s->vt_osc_len - 2] = (char)c;
                s->title[s->vt_osc_len - 1] = 0;
            }
            if (s->vt_osc_len < 250) s->vt_osc_len++;
            return;
        case ST_OSC_ESC: s->vt_state = ST_GROUND; if (c != '\\') { s->vt_state = ST_ESC; vt_putbyte(s, c); } return;
        case ST_STR:
            if (c == 7) s->vt_state = ST_GROUND;
            else if (c == 0x1B) s->vt_state = ST_STR_ESC;
            return;
        case ST_STR_ESC: s->vt_state = ST_GROUND; return;
        default: break;
    }

    if (c < 0x20 || c == 0x7F) {
        s->vt_utf8_left = 0;
        if (c == 0x1B) s->vt_state = ST_ESC;
        else exec_ctl(s, c);
        return;
    }
    if (s->utf8) {
        if (s->vt_utf8_left) {
            if ((c & 0xC0) == 0x80) {
                s->vt_cp = (s->vt_cp << 6) | (c & 0x3F);
                if (--s->vt_utf8_left == 0) print_cp(s, s->vt_cp);
                return;
            }
            s->vt_utf8_left = 0;               /* a broken sequence: show it, then this byte afresh */
            print_cp(s, '?');
        }
        if (c >= 0xF0 && c < 0xF8) { s->vt_cp = c & 7; s->vt_utf8_left = 3; return; }
        if (c >= 0xE0) { s->vt_cp = c & 15; s->vt_utf8_left = 2; return; }
        if (c >= 0xC2) { s->vt_cp = c & 31; s->vt_utf8_left = 1; return; }
        if (c >= 0x80) { print_cp(s, '?'); return; }
    }
    if (s->vt_gfx && c >= 0x60 && c < 0x80) { s->vt_last = c; put_cell(s, gfx_map[c - 0x60]); return; }
    s->vt_last = c;
    put_cell(s, c);
}
