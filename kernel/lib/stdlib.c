#include "stdlib.h"
#include "string.h"
#include "../core/serial.h"

static unsigned long _rand_seed = 1;

int rand(void)
{
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (unsigned)(_rand_seed / 65536) % (RAND_MAX + 1);
}

void srand(unsigned seed)
{
    _rand_seed = seed;
}

int atoi(const char *s)
{
    int n = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

long atol(const char *s)
{
    long n = 0;
    int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

int abs(int j) { return j < 0 ? -j : j; }
long labs(long j) { return j < 0 ? -j : j; }

void exit(int status)
{
    (void)status;
    serial_write("exit() called\n");
    for (;;);
}

void abort(void)
{
    serial_write("abort() called\n");
    for (;;);
}

void *calloc(size_t nmemb, size_t size)
{
    void *ptr = malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);
        free(ptr);
    }
    return new_ptr;
}
