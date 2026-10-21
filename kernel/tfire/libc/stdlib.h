#ifndef TFIRE_STDLIB_H
#define TFIRE_STDLIB_H
/* <stdlib.h> for tFire's third-party engines: the kernel's, and what it lacks (libc_tfire.c) */
#include_next <stdlib.h>
double strtod(const char *s, char **end);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long long llabs(long long v);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
static inline char *getenv(const char *name) { (void)name; return 0; }
#define alloca(n) __builtin_alloca(n)
/* tFire's own allocator (tf_alloc.c) */
void *tf_malloc(size_t n);
void *tf_calloc(size_t n, size_t size);
void *tf_realloc(void *p, size_t n);
void tf_free(void *p);
#define malloc tf_malloc
#define calloc tf_calloc
#define realloc tf_realloc
#define free tf_free
#endif
