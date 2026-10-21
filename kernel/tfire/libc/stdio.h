#ifndef TFIRE_STDIO_H
#define TFIRE_STDIO_H
/* <stdio.h> for tFire's engine and its third-party libraries: the kernel's declarations. libc_tfire.c has a
 * printf family of its own under the same names (with 64-bit integers and floating point), which only tFire's
 * object sees: its symbols stay local to it (the Makefile's TFIRE_API) */
#include_next <stdio.h>
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int fputc(int c, FILE *fp);
int fputs(const char *s, FILE *fp);
int sscanf(const char *s, const char *fmt, ...);
#define putc fputc
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#endif
