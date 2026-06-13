#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *data);
int serial_received(void);
char serial_getchar(void);

#endif
