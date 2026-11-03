#include "paging.h"
#include "terminal.h"
#include "io.h"
#include "string.h"
#include "serial.h"
#include "scheduler.h"

#define VGA_MEMORY ((uint16_t *)P2V(0xB8000))
static uint8_t terminal_color;
static size_t terminal_row;
static size_t terminal_column;
static int terminal_y_offset = 0;

/* Output capture for scripting APIs (T#, MicroPython): while active,
 * terminal_putchar() appends to this buffer instead of touching the
 * screen/serial/surface at all, so a script can run a shell command and
 * get its text back without spamming the visible terminal. */
static char *capture_buf = 0;
static int capture_len = 0;
static int capture_cap = 0;

void terminal_capture_start(char *buf, int max)
{
    capture_buf = buf;
    capture_len = 0;
    capture_cap = max;
    if (capture_cap > 0) capture_buf[0] = 0;
}

void terminal_capture_stop(void)
{
    if (capture_buf && capture_cap > 0) capture_buf[capture_len < capture_cap ? capture_len : capture_cap - 1] = 0;
    capture_buf = 0;
    capture_len = 0;
    capture_cap = 0;
}

static uint8_t make_color(enum vga_color fg, enum vga_color bg)
{
    return fg | bg << 4;
}

static uint16_t make_vgaentry(char c, uint8_t color)
{
    return (uint16_t)c | (uint16_t)color << 8;
}

static int terminal_force_direct = 0;
static int terminal_quiet = 0;

/* the boot screen is up: text still goes to the serial line, not into the VGA memory */
void terminal_set_quiet(int quiet) { terminal_quiet = quiet; }

void terminal_set_force_direct(int enable)
{
    terminal_force_direct = enable;
}

void terminal_fill_screen(uint8_t color)
{
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[y * VGA_WIDTH + x] = make_vgaentry(' ', color);
        }
    }
}

static term_surface_t *current_surface(void)
{
    if (terminal_force_direct) return NULL;
    return (term_surface_t *)task_get_userdata();
}

/* The VGA Attribute Controller defaults to "blink enable" mode, where bit 7
 * of the attribute byte (i.e. background color values 8-15) makes the
 * character blink instead of selecting a bright background. Clear that bit
 * so colors like VGA_WHITE work as a plain background, not a blinking one. */
static void vga_disable_blink(void)
{
    /* Index byte bit 5 (0x20) is the Palette Address Source bit; it must
     * stay set while poking other AC registers or the screen blanks.
     * Do not read AC register 0x10 from 0x3C1 here: that readback hangs
     * VMware SVGA II's legacy VGA path during early boot (the same
     * hardware quirk that vga.c avoids in capture_current_regs()). GRUB
     * leaves the adapter in standard mode 3, whose mode-control value is
     * 0x0C; write the known blink-disabled value 0x04 directly. */
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    outb(0x3C0, 0x04);
}

void terminal_surface_init(term_surface_t *s)
{
    uint8_t color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    s->cur_row = 0;
    s->cur_col = 0;
    s->color = color;
    if (s->cols < 20 || s->cols > TERM_MAXW || s->rows < 5 || s->rows > TERM_MAXH) { s->cols = TERM_SURFACE_W; s->rows = TERM_SURFACE_H; }   /* a cleared window keeps its size */
    for (int i = 0; i < TERM_MAXW * TERM_MAXH; i++)
        s->cells[i] = make_vgaentry(' ', color);
    s->scrollback_head = 0;
    s->scrollback_count = 0;
    s->view_offset = 0;
    /* a terminal in cooked mode, as Linux starts one (what a program's termios calls change) */
    tty_t *t = &s->tty;
    t->c_iflag = 0x0502;                /* ICRNL IXON BRKINT */
    t->c_oflag = 0x0005;                /* OPOST ONLCR */
    t->c_cflag = 0x04BF;                /* CS8 CREAD HUPCL B38400 */
    t->c_lflag = 0x8A3B;                /* ISIG ICANON ECHO ECHOE ECHOK ECHOCTL ECHOKE IEXTEN */
    for (int i = 0; i < TTY_NCC; i++) t->c_cc[i] = 0;
    t->c_cc[0] = 3; t->c_cc[1] = 0x1C; t->c_cc[2] = 0x7F; t->c_cc[3] = 0x15; t->c_cc[4] = 4;
    t->c_cc[6] = 1; t->c_cc[8] = 0x11; t->c_cc[9] = 0x13; t->c_cc[10] = 0x1A; t->c_cc[12] = 0x12;
    t->c_cc[13] = 0x0F; t->c_cc[14] = 0x17; t->c_cc[15] = 0x16;
    t->ws_row = (uint16_t)s->rows; t->ws_col = (uint16_t)s->cols;
    t->inq_head = t->inq_tail = 0; t->line_len = 0; t->line_done = 0; t->eof_pending = 0; t->fg_pid = 0; t->fg_set = 0;
    vt_init(s);
}

static void scrollback_push(term_surface_t *s, const uint16_t *row);

/* A window's new size. The text stays where it is; when the window gets shorter than the cursor's row, the top
 * lines go to the scrollback (on the main screen) so the cursor stays in view. */
void terminal_surface_resize(term_surface_t *s, int cols, int rows)
{
    if (cols < 20) cols = 20;
    if (cols > TERM_MAXW) cols = TERM_MAXW;
    if (rows < 5) rows = 5;
    if (rows > TERM_MAXH) rows = TERM_MAXH;
    if (cols == s->cols && rows == s->rows) return;
    uint16_t blank = make_vgaentry(' ', make_color(VGA_LIGHT_GREY, VGA_BLACK));
    if (rows < s->rows && !s->alt_screen && s->cur_row >= rows) {
        int drop = s->cur_row - rows + 1;
        for (int k = 0; k < drop; k++) {
            scrollback_push(s, &s->cells[0]);
            for (int y = 1; y < s->rows; y++)
                for (int x = 0; x < s->cols; x++) s->cells[(y - 1) * TERM_MAXW + x] = s->cells[y * TERM_MAXW + x];
            for (int x = 0; x < s->cols; x++) s->cells[(s->rows - 1) * TERM_MAXW + x] = blank;
        }
        s->cur_row -= drop;
    }
    s->cols = cols; s->rows = rows;
    for (int pass = 0; pass < 2; pass++) {                  /* the screen and the other (alternate) one: blank outside the new size */
        uint16_t *c = pass ? s->alt_cells : s->cells;
        for (int y = 0; y < TERM_MAXH; y++)
            for (int x = 0; x < TERM_MAXW; x++)
                if (y >= rows || x >= cols) c[y * TERM_MAXW + x] = blank;
    }
    if (s->cur_row >= rows) s->cur_row = rows - 1;
    if (s->cur_col >= cols) s->cur_col = cols - 1;
    s->top = 0; s->bottom = rows - 1;
    s->wrap_pending = 0;
    s->view_offset = 0;
    s->tty.ws_row = (uint16_t)rows; s->tty.ws_col = (uint16_t)cols;
}

const uint16_t *terminal_scrollback_row(const term_surface_t *s, int lines_back)
{
    if (lines_back < 1 || lines_back > s->scrollback_count) return 0;
    int idx = (s->scrollback_head - lines_back + TERM_SCROLLBACK_LINES) % TERM_SCROLLBACK_LINES;
    return s->scrollback[idx];
}

static void scrollback_push(term_surface_t *s, const uint16_t *row)
{
    for (int x = 0; x < s->cols; x++) s->scrollback[s->scrollback_head][x] = row[x];
    for (int x = s->cols; x < TERM_MAXW; x++) s->scrollback[s->scrollback_head][x] = make_vgaentry(' ', s->color);
    s->scrollback_head = (s->scrollback_head + 1) % TERM_SCROLLBACK_LINES;
    if (s->scrollback_count < TERM_SCROLLBACK_LINES) s->scrollback_count++;
}

void terminal_set_y_offset(int offset)
{
    terminal_y_offset = offset;
}

int terminal_get_y_offset(void)
{
    return terminal_y_offset;
}

void terminal_init(void)
{
    vga_disable_blink();
    terminal_color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_row = 0;
    terminal_column = 0;
    terminal_y_offset = 0;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = make_vgaentry(' ', terminal_color);
        }
    }
}

void terminal_clear(void)
{
    term_surface_t *s = current_surface();
    if (s) {
        terminal_surface_init(s);
        return;
    }
    terminal_row = 0;
    terminal_column = 0;
    if (terminal_quiet) return;
    for (int y = terminal_y_offset; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = make_vgaentry(' ', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color)
{
    term_surface_t *s = current_surface();
    if (s) { vt_set_color(s, color); return; }
    terminal_color = color;
}

static void surface_scroll(term_surface_t *s)
{
    scrollback_push(s, &s->cells[0]);
    for (int y = 1; y < s->rows; y++) {
        for (int x = 0; x < s->cols; x++)
            s->cells[(y - 1) * TERM_MAXW + x] = s->cells[y * TERM_MAXW + x];
    }
    for (int x = 0; x < s->cols; x++)
        s->cells[(s->rows - 1) * TERM_MAXW + x] = make_vgaentry(' ', s->color);
    s->cur_row = s->rows - 1;
}

void terminal_scroll(void)
{
    term_surface_t *s = current_surface();
    if (s) { surface_scroll(s); return; }
    for (int y = terminal_y_offset + 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[(y-1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA_MEMORY[(VGA_HEIGHT-1) * VGA_WIDTH + x] = make_vgaentry(' ', terminal_color);
    }
    terminal_row = VGA_HEIGHT - 1 - terminal_y_offset;
}

term_surface_t *terminal_current_surface(void) { return current_surface(); }

void terminal_putchar(char c)
{
    if (capture_buf) {
        if (capture_len < capture_cap - 1) capture_buf[capture_len++] = c;
        return;
    }

    serial_putchar(c);

    /* a surface takes the text always; "quiet" (the boot screen is up) is about the VGA memory */
    term_surface_t *s = current_surface();
    if (s) { vt_putbyte(s, (uint8_t)c); return; }
    if (terminal_quiet) return;

    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
    } else if (c == '\t') {
        terminal_column = (terminal_column + 8) & ~7;
    } else if (c == '\r') {
        terminal_column = 0;
    } else if (c == '\b') {
        if (terminal_column > 0) terminal_column--;
    } else {
        const size_t index = (terminal_row + terminal_y_offset) * VGA_WIDTH + terminal_column;
        VGA_MEMORY[index] = make_vgaentry(c, terminal_color);
        terminal_column++;
    }
    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;
    }
    if ((int)terminal_row >= VGA_HEIGHT - terminal_y_offset) {
        terminal_scroll();
    }
}

void terminal_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char *data)
{
    terminal_write(data, strlen(data));
}

void terminal_setpos(size_t x, size_t y)
{
    term_surface_t *s = current_surface();
    if (s) {
        if ((int)x < s->cols) s->cur_col = (int)x;
        if ((int)y < s->rows) s->cur_row = (int)y;
        s->wrap_pending = 0;
        return;
    }
    if (x < VGA_WIDTH) terminal_column = x;
    if (y < VGA_HEIGHT) terminal_row = y;
}

void terminal_getpos(size_t *x, size_t *y)
{
    term_surface_t *s = current_surface();
    if (s) {
        if (x) *x = (size_t)s->cur_col;
        if (y) *y = (size_t)s->cur_row;
        return;
    }
    if (x) *x = terminal_column;
    if (y) *y = terminal_row;
}
