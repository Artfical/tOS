#ifndef TFIRE_STRING_H
#define TFIRE_STRING_H
/* <string.h> for tFire's third-party engines: the kernel's, and what it lacks (libc_tfire.c) */
#include_next <string.h>
void *memchr(const void *s, int c, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strrchr(const char *s, int c);
char *strncat(char *d, const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
size_t strnlen(const char *s, size_t n);
static inline const char *strerror(int e) { (void)e; return "error"; }
#endif
