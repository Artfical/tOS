#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KBD_LAYOUT_US  0
#define KBD_LAYOUT_TRQ 1

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_getchar_ring3(void);
void keyboard_readline(char *buf, int max);
int keyboard_data_available(void);
int keyboard_try_getchar(char *out);
int keyboard_yesno(void);
void keyboard_set_layout(int layout);
int keyboard_get_layout(void);
int keyboard_get_special(void);
int keyboard_shift_held(void);
int keyboard_shift_seen(void);         /* Shift was pressed since tOS started (held while booting) */
int keyboard_ctrl_held(void);
int keyboard_alt_held(void);

/* Raw press/release events (see keyboard.c) -- pops one event into
 * key/pressed (key is either an RAWKEY_ arrow code from 1-4 or a
 * plain lowercase-scancode-table ASCII value; 27 for Escape), returns
 * 1 if an event was popped, 0 if the queue is empty. Needed by things
 * that must know when a key is released, not just pressed (games),
 * which nothing else in this file tracks. */
int keyboard_get_raw_event(uint8_t *key, int *pressed);
#define RAWKEY_LEFT  1
#define RAWKEY_RIGHT 2
#define RAWKEY_UP    3
#define RAWKEY_DOWN  4

/* In xterm mode (a Linux program owns the terminal) a key that is not a character reaches the queue as
 * 0xFF, one of these codes, a modifier mask (1 shift, 2 alt, 4 ctrl): kernel/fs/tty.c makes the sequence. */
#define XKEY_UP    1
#define XKEY_DOWN  2
#define XKEY_RIGHT 3
#define XKEY_LEFT  4
#define XKEY_HOME  5
#define XKEY_END   6
#define XKEY_PGUP  7
#define XKEY_PGDN  8
#define XKEY_INS   9
#define XKEY_DEL   10
#define XKEY_F1    11    /* F1..F12 are 11..22 */
#define XKEY_F5    15
#define XKEY_F12   22
#define XKEY_F4    14
void keyboard_set_xterm(int on);
int  keyboard_tty_poll(uint8_t *out);          /* the next byte for the focused window's program, without waiting */

extern volatile int interrupt_char;
extern void (*interrupt_callback)(void);

#endif
