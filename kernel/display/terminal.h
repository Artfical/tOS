#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#define TERM_SURFACE_W 80
#define TERM_SURFACE_H 23
#define TERM_SCROLLBACK_LINES 100

/* When bound (via task_set_userdata) on the current task, all terminal_*
 * calls made by that task read/write this surface instead of the real
 * VGA buffer. Must remain the first member of any struct passed as
 * userdata so window_t (wm.c) and term_surface_t are interchangeable. */
/* What the terminal is told by its program (termios) and what its keyboard has produced for it */
#define TTY_NCC 19
#define TTY_INQ 512
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_cc[TTY_NCC];
    uint16_t ws_row, ws_col;
    uint8_t inq[TTY_INQ];               /* bytes waiting for the program (after the line discipline) */
    uint16_t inq_head, inq_tail;
    uint8_t line[256];                  /* a line being edited (canonical mode) */
    uint16_t line_len;
    uint8_t line_done;                  /* the line is complete: inq has it */
    uint8_t eof_pending;
    uint32_t fg_pid;                    /* the foreground process (^C goes to it) */
} tty_t;

typedef struct {
    uint16_t cells[TERM_SURFACE_W * TERM_SURFACE_H];
    int cur_row;
    int cur_col;
    uint8_t color;
    /* Lines pushed off the top by scrolling, oldest overwritten once full.
     * scrollback_head is the next write slot; view_offset is how many
     * lines the window manager is currently showing scrolled back from
     * live (0 = live), driven by the scrollbar in wm.c. */
    uint16_t scrollback[TERM_SCROLLBACK_LINES][TERM_SURFACE_W];
    int scrollback_head;
    int scrollback_count;
    int view_offset;
    /* an xterm-style terminal (vt.c): escape sequences, colours, the alternate screen... */
    uint8_t vt_state, vt_nparams, vt_prefix, vt_inter, vt_gfx, vt_utf8_left, vt_osc_len;
    uint32_t vt_cp, vt_last;
    int vt_param[16];
    uint8_t fg, bg, sgr;                /* sgr: 1 bold, 2 reverse; color is derived from these */
    int saved_row, saved_col;
    uint8_t saved_fg, saved_bg, saved_sgr, saved_gfx;
    int top, bottom;                    /* scroll region, rows */
    uint8_t wrap_pending, autowrap, cursor_visible, alt_screen, app_cursor, bracketed_paste;
    uint8_t mouse_mode, mouse_sgr, insert_mode, origin, utf8, xterm_keys;
    uint8_t hidden;                     /* a log no window shows (a service's): it has no keyboard */
    int alt_saved_row, alt_saved_col;
    uint16_t alt_cells[TERM_SURFACE_W * TERM_SURFACE_H];
    char title[48];
    uint8_t reply[48];                  /* answers to the program's queries (cursor position...): its input */
    uint8_t reply_len;
    tty_t tty;
} term_surface_t;

void terminal_surface_init(term_surface_t *s);
/* vt.c: one byte of a program's output into a surface */
void vt_init(term_surface_t *s);
void vt_putbyte(term_surface_t *s, uint8_t c);
void vt_reset_modes(term_surface_t *s);          /* the program is gone: main screen, cursor, colours, no mouse */
void vt_set_color(term_surface_t *s, uint8_t attr);   /* legacy callers that set a VGA attribute byte */
term_surface_t *terminal_current_surface(void);  /* the surface the current task writes to, or 0 (the VGA screen) */
const uint16_t *terminal_scrollback_row(const term_surface_t *s, int lines_back);

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
void terminal_capture_start(char *buf, int max);
void terminal_capture_stop(void);

/* While enabled, every terminal_* call bypasses whatever window
 * surface the current task has bound (if any) and writes straight to
 * the physical VGA buffer -- used by the crash screen so it always
 * takes over the real display, even if the fault happened while some
 * GUI window's surface was the active render target. */
void terminal_set_force_direct(int enable);
void terminal_set_quiet(int quiet);          /* text to serial only (the boot screen is showing) */
void terminal_fill_screen(uint8_t color);

#endif
