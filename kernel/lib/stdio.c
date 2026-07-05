#include <stdarg.h>
#include <stddef.h>
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"
#include "../fs/vfs.h"

FILE _stdin  = { .fd = 0, .flags = 0 };
FILE _stdout = { .fd = 1, .flags = 0 };
FILE _stderr = { .fd = 2, .flags = 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

static void print_char(char **out, size_t *n, char c)
{
    if (*n > 1) {
        **out = c;
        (*out)++;
        (*n)--;
    }
}

static void print_pad(char **out, size_t *n, char pad, int count)
{
    while (count-- > 0)
        print_char(out, n, pad);
}

static void print_num(char **out, size_t *n, uint32_t val, int base, int sign, int width, int zero, int left, int prec)
{
    char buf[36];
    char *p = buf + sizeof(buf);
    int neg = 0;
    if (sign && (int32_t)val < 0) {
        neg = 1;
        val = -(int32_t)val;
    }
    *--p = 0;
    if (val == 0) *--p = '0';
    else while (val > 0) {
        int d = val % base;
        *--p = d < 10 ? '0' + d : 'a' + d - 10;
        val /= base;
    }
    /* A precision on an integer conversion (e.g. "%.3d") means "at
     * least this many digits, zero-padded", independent of (and
     * overriding) the width/'0' flag -- used for e.g. DOOM's
     * "STCFN%.3d" lump-name formatting. */
    if (prec >= 0) {
        int digits = (buf + sizeof(buf) - 1) - p;
        while (digits < prec && p > buf + 1) { *--p = '0'; digits++; }
        zero = 0;
    }
    int len = (buf + sizeof(buf) - 1) - p + neg;
    if (left) {
        if (neg) print_char(out, n, '-');
        while (*p) print_char(out, n, *p++);
        print_pad(out, n, ' ', width - len);
        return;
    }
    if (!zero) {
        if (neg) print_char(out, n, '-');
        print_pad(out, n, ' ', width - len);
    } else {
        print_pad(out, n, ' ', width - len);
        if (neg) print_char(out, n, '-');
    }
    while (*p) print_char(out, n, *p++);
}

static void print_str_padded(char **out, size_t *n, const char *s, int width, int left, int prec)
{
    if (!s) s = "(null)";
    int len = (int)strlen(s);
    /* A precision on %s (e.g. "%.8s") caps how many characters are
     * printed, unlike every other flag/width here which only pads. */
    if (prec >= 0 && len > prec) len = prec;
    if (!left) print_pad(out, n, ' ', width - len);
    for (int i = 0; i < len; i++) print_char(out, n, s[i]);
    if (left) print_pad(out, n, ' ', width - len);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    char *start = buf;
    if (n == 0) return 0;
    while (*fmt && n > 1) {
        if (*fmt != '%') {
            print_char(&buf, &n, *fmt);
            fmt++;
            continue;
        }
        fmt++;
        int width = 0, zero = 0, left = 0, prec = -1;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left = 1;
            else zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                prec = prec * 10 + (*fmt - '0');
                fmt++;
            }
        }
        switch (*fmt) {
            case 'd':
            case 'i':
            case 'u': {
                int s = (*fmt == 'd' || *fmt == 'i') ? 1 : 0;
                print_num(&buf, &n, va_arg(ap, uint32_t), 10, s, width, zero, left, prec);
                break;
            }
            case 'x':
            case 'p': print_num(&buf, &n, va_arg(ap, uint32_t), 16, 0, width, zero, left, prec); break;
            case 'X': {
                char *p = buf;
                print_num(&buf, &n, va_arg(ap, uint32_t), 16, 0, width, zero, left, prec);
                while (p < buf) { if (*p >= 'a' && *p <= 'f') *p -= 32; p++; }
                break;
            }
            case 's': print_str_padded(&buf, &n, va_arg(ap, const char *), width, left, prec); break;
            case 'c': print_char(&buf, &n, va_arg(ap, int)); break;
            case '%': print_char(&buf, &n, '%'); break;
            default: print_char(&buf, &n, '%'); print_char(&buf, &n, *fmt); break;
        }
        fmt++;
    }
    *buf = 0;
    return buf - start;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

#include "../core/serial.h"
#include "../display/terminal.h"

int putchar(int c)
{
    /* terminal_putchar() already mirrors to serial_putchar() itself
     * (see terminal.c) -- calling it again here duplicated every
     * character printf() ever wrote to the serial log. Never noticed
     * before since almost everything else in this codebase writes via
     * terminal_writestring() directly instead of printf(); DOOM's
     * DEH_printf (#define'd straight to printf) is what first made it
     * visible. */
    if (c == '\n') terminal_putchar('\r');
    terminal_putchar(c);
    return c;
}

int puts(const char *s)
{
    while (*s) putchar(*s++);
    putchar('\n');
    return 1;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; buf[i]; i++)
        putchar(buf[i]);
    return r;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fp == stdout || fp == stderr) {
        for (int i = 0; buf[i]; i++)
            putchar(buf[i]);
    }
    return r;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    if (mode[0] == 'r') flags = 0;
    else if (mode[0] == 'w') flags = VFS_WRONLY | VFS_CREAT | VFS_TRUNC;
    else if (mode[0] == 'a') flags = VFS_WRONLY | VFS_CREAT | VFS_APPEND;
    int fd = vfs_open(path, flags);
    if (fd < 0) { errno = -fd; return 0; }
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { vfs_close(fd); return 0; }
    fp->fd = fd;
    fp->flags = flags;
    fp->eof = 0;
    fp->error = 0;
    return fp;
}

int fclose(FILE *fp)
{
    if (!fp) return EOF;
    int r = vfs_close(fp->fd);
    free(fp);
    return r < 0 ? EOF : 0;
}
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    int r = vfs_read(fp->fd, ptr, size * nmemb);
    if (r < 0) { fp->error = 1; return 0; }
    if (r == 0) fp->eof = 1;
    return r / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    int r = vfs_write(fp->fd, ptr, size * nmemb);
    if (r < 0) { fp->error = 1; return 0; }
    return r / size;
}

int fseek(FILE *fp, long offset, int whence)
{
    int r = vfs_lseek(fp->fd, offset, whence);
    if (r < 0) { fp->error = 1; return -1; }
    fp->eof = 0;
    return 0;
}

long ftell(FILE *fp)
{
    return vfs_lseek(fp->fd, 0, VFS_SEEK_CUR);
}

int feof(FILE *fp) { return fp->eof; }
int ferror(FILE *fp) { return fp->error; }
int fflush(FILE *fp) { (void)fp; return 0; }

int fgetc(FILE *fp)
{
    unsigned char c;
    int r = vfs_read(fp->fd, &c, 1);
    if (r <= 0) { fp->eof = 1; return EOF; }
    return c;
}

char *fgets(char *s, int size, FILE *fp)
{
    int i = 0;
    while (i < size - 1) {
        unsigned char c;
        int r = vfs_read(fp->fd, &c, 1);
        if (r <= 0) { fp->eof = 1; break; }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return (i == 0 && feof(fp)) ? 0 : s;
}
