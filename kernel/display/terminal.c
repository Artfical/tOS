#include "terminal.h"
#include "io.h"
#include "string.h"
#include "serial.h"
#include "scheduler.h"

static uint16_t *const VGA_MEMORY = (uint16_t *)0xB8000;
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

static term_surface_t *current_surface(void)
{
    return (term_surface_t *)task_get_userdata();
}

/* The VGA Attribute Controller defaults to "blink enable" mode, where bit 7
 * of the attribute byte (i.e. background color values 8-15) makes the
 * character blink instead of selecting a bright background. Clear that bit
 * so colors like VGA_WHITE work as a plain background, not a blinking one. */
static void vga_disable_blink(void)
{
    /* Index byte bit 5 (0x20) is the Palette Address Source bit; it must
     * stay set while poking other AC registers or the screen blanks. */
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    uint8_t mode = inb(0x3C1);
    mode &= ~0x08;
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    outb(0x3C0, mode);
}

void terminal_surface_init(term_surface_t *s)
{
    uint8_t color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    s->cur_row = 0;
    s->cur_col = 0;
    s->color = color;
    for (int i = 0; i < TERM_SURFACE_W * TERM_SURFACE_H; i++)
        s->cells[i] = make_vgaentry(' ', color);
    s->scrollback_head = 0;
    s->scrollback_count = 0;
    s->view_offset = 0;
}

const uint16_t *terminal_scrollback_row(const term_surface_t *s, int lines_back)
{
    if (lines_back < 1 || lines_back > s->scrollback_count) return 0;
    int idx = (s->scrollback_head - lines_back + TERM_SCROLLBACK_LINES) % TERM_SCROLLBACK_LINES;
    return s->scrollback[idx];
}

static void scrollback_push(term_surface_t *s, const uint16_t *row)
{
    for (int x = 0; x < TERM_SURFACE_W; x++) s->scrollback[s->scrollback_head][x] = row[x];
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
    if (s) { s->color = color; return; }
    terminal_color = color;
}

static void surface_scroll(term_surface_t *s)
{
    scrollback_push(s, &s->cells[0]);
    for (int y = 1; y < TERM_SURFACE_H; y++) {
        for (int x = 0; x < TERM_SURFACE_W; x++)
            s->cells[(y - 1) * TERM_SURFACE_W + x] = s->cells[y * TERM_SURFACE_W + x];
    }
    for (int x = 0; x < TERM_SURFACE_W; x++)
        s->cells[(TERM_SURFACE_H - 1) * TERM_SURFACE_W + x] = make_vgaentry(' ', s->color);
    s->cur_row = TERM_SURFACE_H - 1;
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

static void surface_putchar(term_surface_t *s, char c)
{
    s->view_offset = 0;
    if (c == '\n') {
        s->cur_col = 0;
        s->cur_row++;
    } else if (c == '\t') {
        s->cur_col = (s->cur_col + 8) & ~7;
    } else if (c == '\r') {
        s->cur_col = 0;
    } else if (c == '\b') {
        if (s->cur_col > 0) s->cur_col--;
    } else {
        s->cells[s->cur_row * TERM_SURFACE_W + s->cur_col] = make_vgaentry(c, s->color);
        s->cur_col++;
    }
    if (s->cur_col >= TERM_SURFACE_W) {
        s->cur_col = 0;
        s->cur_row++;
    }
    if (s->cur_row >= TERM_SURFACE_H) {
        surface_scroll(s);
    }
}

void terminal_putchar(char c)
{
    if (capture_buf) {
        if (capture_len < capture_cap - 1) capture_buf[capture_len++] = c;
        return;
    }

    serial_putchar(c);

    term_surface_t *s = current_surface();
    if (s) { surface_putchar(s, c); return; }

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
        if ((int)x < TERM_SURFACE_W) s->cur_col = (int)x;
        if ((int)y < TERM_SURFACE_H) s->cur_row = (int)y;
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
