#include "terminal.h"
#include "io.h"
#include "string.h"
#include "serial.h"

static uint16_t *const VGA_MEMORY = (uint16_t *)0xB8000;
static uint8_t terminal_color;
static size_t terminal_row;
static size_t terminal_column;

static uint8_t make_color(enum vga_color fg, enum vga_color bg)
{
    return fg | bg << 4;
}

static uint16_t make_vgaentry(char c, uint8_t color)
{
    return (uint16_t)c | (uint16_t)color << 8;
}

void terminal_init(void)
{
    terminal_color = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_row = 0;
    terminal_column = 0;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = make_vgaentry(' ', terminal_color);
        }
    }
}

void terminal_clear(void)
{
    terminal_row = 0;
    terminal_column = 0;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = make_vgaentry(' ', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color)
{
    terminal_color = color;
}

void terminal_scroll(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[(y-1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        VGA_MEMORY[(VGA_HEIGHT-1) * VGA_WIDTH + x] = make_vgaentry(' ', terminal_color);
    }
    terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c)
{
    serial_putchar(c);
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
        const size_t index = terminal_row * VGA_WIDTH + terminal_column;
        VGA_MEMORY[index] = make_vgaentry(c, terminal_color);
        terminal_column++;
    }
    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;
    }
    if (terminal_row >= VGA_HEIGHT) {
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
    if (x < VGA_WIDTH) terminal_column = x;
    if (y < VGA_HEIGHT) terminal_row = y;
}

void terminal_getpos(size_t *x, size_t *y)
{
    if (x) *x = terminal_column;
    if (y) *y = terminal_row;
}
