/* Wraps kernel/lib/stdlib.h -- see string.h in this same directory
 * for why (#include_next) and doom_compat.c for the implementations. */
#include_next <stdlib.h>

#ifndef DOOM_STDLIB_COMPAT_H
#define DOOM_STDLIB_COMPAT_H

char *getenv(const char *name);
int system(const char *command);
double atof(const char *s);

#endif
