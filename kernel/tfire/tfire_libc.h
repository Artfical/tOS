#ifndef TFIRE_LIBC_H
#define TFIRE_LIBC_H
/* the C library for tFire's engine: the kernel's own (or the host's, for the test harness) */
#include <stddef.h>
#include <stdint.h>
#ifdef TFIRE_HOST
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#else
#include "string.h"
#include "stdlib.h"
#include "memory.h"
#include "ctype.h"
#include "stdio.h"
#include "strings.h"
void *memchr(const void *s, int c, size_t n);
#endif
#ifndef TFIRE_HOST
/* tFire's own allocator (libc/tf_alloc.c): what it frees may also be the kernel's */
void *tf_malloc(size_t n);
void *tf_calloc(size_t n, size_t size);
void *tf_realloc(void *p, size_t n);
void tf_free(void *p);
char *tf_strdup(const char *s);
#define malloc tf_malloc
#define calloc tf_calloc
#define realloc tf_realloc
#define free tf_free
#define strdup tf_strdup
#endif
static inline char *strndup_(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
#endif
