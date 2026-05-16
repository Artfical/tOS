#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define RAND_MAX 32767

int atoi(const char *s);
long atol(const char *s);
int abs(int j);
long labs(long j);
int rand(void);
void srand(unsigned seed);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status);
void abort(void);

#endif
