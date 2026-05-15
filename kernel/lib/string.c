#include "string.h"

size_t strlen(const char *str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strchr(const char *str, int c)
{
    while (*str) {
        if (*str == (char)c) return (char *)str;
        str++;
    }
    return (c == '\0') ? (char *)str : NULL;
}

void *memset(void *ptr, int value, size_t num)
{
    unsigned char *p = (unsigned char *)ptr;
    for (size_t i = 0; i < num; i++)
        p[i] = (unsigned char)value;
    return ptr;
}

void *memcpy(void *dest, const void *src, size_t num)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < num; i++)
        d[i] = s[i];
    return dest;
}

void *memmove(void *dest, const void *src, size_t num)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < num; i++) d[i] = s[i];
    } else {
        for (size_t i = num; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t num)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < num; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

static char *strtok_ptr = NULL;

char *strtok(char *str, const char *delim)
{
    if (str) strtok_ptr = str;
    if (!strtok_ptr) return NULL;

    char *start = strtok_ptr;
    while (*strtok_ptr) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) { if (*strtok_ptr == *d) { is_delim = 1; break; } d++; }
        if (is_delim) {
            *strtok_ptr = '\0';
            strtok_ptr++;
            if (strtok_ptr == start + 1 && *start == '\0') { start = strtok_ptr; continue; }
            return start;
        }
        strtok_ptr++;
    }
    strtok_ptr = NULL;
    return (*start) ? start : NULL;
}

unsigned long atoul(const char *str)
{
    unsigned long result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}
