/* Wraps kernel/lib/string.h (found via #include_next once this
 * directory's earlier -I position is skipped past) and adds the
 * handful of extra string functions DOOM's source calls that tOS's
 * libc doesn't happen to provide -- see doom_compat.c. */
#include_next <string.h>

#ifndef DOOM_STRING_COMPAT_H
#define DOOM_STRING_COMPAT_H

char *strdup(const char *s);
char *strrchr(const char *s, int c);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
