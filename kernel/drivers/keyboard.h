#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
char keyboard_getchar(void);
void keyboard_readline(char *buf, int max);
int keyboard_data_available(void);
int keyboard_yesno(int timeout_seconds);

#endif
