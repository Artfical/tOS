/* Wraps kernel/lib/stdio.h in extern "C" -- see string.h in this same
 * directory for why. vfprintf()/sscanf() are declared but NOT defined
 * again here: kernel/doom/port/doom_compat.c already implements both
 * as plain (C-linkage) global symbols (see kernel/wolf3d/port/
 * stdlib.h for the same reasoning applied to getenv()/system()/
 * atof()). */
#ifndef WOLF3D_STDIO_COMPAT_H
#define WOLF3D_STDIO_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <stdio.h>
#include <stdarg.h>

int vfprintf(FILE *fp, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifdef __cplusplus
}
#endif

#endif
