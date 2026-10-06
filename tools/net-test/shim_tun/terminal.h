#ifndef SHIM_TERMINAL_H
#define SHIM_TERMINAL_H
#ifdef TERM_PRINT
#include <stdio.h>
static inline void terminal_writestring(const char *s) { fputs(s, stdout); }
static inline void terminal_putchar(char c) { putchar(c); }
#else
static inline void terminal_writestring(const char *s) { (void)s; }
static inline void terminal_putchar(char c) { (void)c; }
#endif
#endif
