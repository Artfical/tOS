#ifndef KEYBOARD_H
#define KEYBOARD_H

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

extern volatile int interrupt_char;
extern void (*interrupt_callback)(void);

#endif
