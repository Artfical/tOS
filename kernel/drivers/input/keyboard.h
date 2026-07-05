#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KBD_LAYOUT_US  0
#define KBD_LAYOUT_TRQ 1

void keyboard_init(void);
char keyboard_getchar(void);
void keyboard_readline(char *buf, int max);
int keyboard_data_available(void);
int keyboard_yesno(void);
void keyboard_set_layout(int layout);
int keyboard_get_layout(void);
int keyboard_get_special(void);
int keyboard_shift_held(void);
int keyboard_ctrl_held(void);

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

extern volatile int interrupt_char;
extern void (*interrupt_callback)(void);

#endif
