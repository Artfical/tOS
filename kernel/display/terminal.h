#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#define TERM_SURFACE_W 80
#define TERM_SURFACE_H 23

/* When bound (via task_set_userdata) on the current task, all terminal_*
 * calls made by that task read/write this surface instead of the real
 * VGA buffer. Must remain the first member of any struct passed as
 * userdata so window_t (wm.c) and term_surface_t are interchangeable. */
typedef struct {
    uint16_t cells[TERM_SURFACE_W * TERM_SURFACE_H];
    int cur_row;
    int cur_col;
    uint8_t color;
} term_surface_t;

void terminal_surface_init(term_surface_t *s);

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15,
};

void terminal_init(void);
void terminal_clear(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char *data, size_t size);
void terminal_writestring(const char *data);
void terminal_setpos(size_t x, size_t y);
void terminal_getpos(size_t *x, size_t *y);
void terminal_scroll(void);
void terminal_set_y_offset(int offset);
int terminal_get_y_offset(void);

#endif
