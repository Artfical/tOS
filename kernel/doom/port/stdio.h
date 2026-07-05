/* Wraps kernel/lib/stdio.h -- see string.h in this same directory for
 * why (#include_next) and doom_compat.c for the implementations. */
#include_next <stdio.h>

#ifndef DOOM_STDIO_COMPAT_H
#define DOOM_STDIO_COMPAT_H

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#endif
