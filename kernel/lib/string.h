#ifndef STRING_H
#define STRING_H

#include <stddef.h>

size_t strlen(const char *str);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *str, int c);
void *memset(void *ptr, int value, size_t num);
void *memcpy(void *dest, const void *src, size_t num);
void *memmove(void *dest, const void *src, size_t num);
int memcmp(const void *a, const void *b, size_t num);
char *strtok(char *str, const char *delim);
unsigned long atoul(const char *str);
char *strstr(const char *haystack, const char *needle);

#endif
