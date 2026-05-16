#ifndef _STDIO_H
#define _STDIO_H

#include <stdint.h>
#include <stddef.h>

#define EOF (-1)

typedef struct {
    int fd;
    int flags;
    int eof;
    int error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
int fflush(FILE *fp);
int fgetc(FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int fprintf(FILE *fp, const char *fmt, ...);

#endif
