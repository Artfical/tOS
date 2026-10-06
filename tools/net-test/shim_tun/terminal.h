#ifndef SHIM_TERMINAL_H
#define SHIM_TERMINAL_H
static inline void terminal_writestring(const char *s) { (void)s; }
static inline void terminal_putchar(char c) { (void)c; }
#endif
