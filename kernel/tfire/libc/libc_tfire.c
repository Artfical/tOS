/*
 * The bits of the C library that tFire's third-party engines (Duktape, NetSurf's libcss, libhubbub,
 * libparserutils, libwapcaplet) use and the kernel's own lib/ does not have.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>
#include <strings.h>
#include <setjmp.h>
#include <math.h>

void *tf_malloc(size_t n);
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = tf_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    const char *lo = base;
    while (n) {
        const char *mid = lo + (n / 2) * size;
        int r = cmp(key, mid);
        if (!r) return (void *)mid;
        if (r > 0) { lo = mid + size; n -= n / 2 + 1; }
        else n /= 2;
    }
    return 0;
}

static int digit_of(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* sscanf: integers (%d %i %u %x %o, with widths), %p, %s, %c, %n, %% and literal text (white space matches any) */
int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int got = 0;
    const char *p = s;
    for (; *fmt; fmt++) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; continue; }
        if (*fmt != '%') { if (*p != *fmt) break; p++; continue; }
        fmt++;
        if (*fmt == '%') { if (*p != '%') break; p++; continue; }
        int skip = 0, width = 0, lng = 0;
        if (*fmt == '*') { skip = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
        char c = *fmt;
        if (!c) break;
        if (c == 'n') { if (!skip) *va_arg(ap, int *) = (int)(p - s); continue; }
        if (c != 'c') while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            int base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : 10;
            const char *q = p;
            int neg = 0, digits = 0;
            if ((*q == '-' || *q == '+') && base == 10 && (!width || width > 1)) { neg = *q == '-'; q++; }
            if (base == 16 && q[0] == '0' && (q[1] == 'x' || q[1] == 'X') && (!width || width > 2)) q += 2;
            unsigned long long v = 0;
            while (*q && (!width || q - p < width)) {
                int d = digit_of((unsigned char)*q);
                if (d >= base) break;
                v = v * (unsigned)base + (unsigned)d;
                q++;
                digits++;
            }
            if (!digits) break;
            p = q;
            if (!skip) {
                if (c == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else if (lng >= 2) *va_arg(ap, long long *) = neg ? -(long long)v : (long long)v;
                else if (lng == 1) *va_arg(ap, long *) = neg ? -(long)v : (long)v;
                else *va_arg(ap, int *) = neg ? -(int)v : (int)v;
                got++;
            }
        } else if (c == 's') {
            char *o = skip ? 0 : va_arg(ap, char *);
            int k = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && (!width || k < width)) { if (o) o[k] = *p; k++; p++; }
            if (o) { o[k] = 0; got++; }
        } else if (c == 'c') {
            int n = width ? width : 1;
            char *o = skip ? 0 : va_arg(ap, char *);
            for (int k = 0; k < n && *p; k++, p++) if (o) o[k] = *p;
            if (o) got++;
        } else break;
    }
    va_end(ap);
    return got;
}

/* 64-bit signed division, which gcc calls for "long long" on i386 (the kernel has the unsigned ones) */
extern uint64_t __udivdi3(uint64_t a, uint64_t b);
extern uint64_t __umoddi3(uint64_t a, uint64_t b);

int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t q = __udivdi3(a < 0 ? -(uint64_t)a : (uint64_t)a, b < 0 ? -(uint64_t)b : (uint64_t)b);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t r = __umoddi3(a < 0 ? -(uint64_t)a : (uint64_t)a, b < 0 ? -(uint64_t)b : (uint64_t)b);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}

/* setjmp/longjmp: jmp_buf holds ebx, esi, edi, ebp, the caller's esp and the return address */
__asm__(
    ".text\n"
    ".global setjmp\n"
    ".type setjmp, @function\n"
    "setjmp:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %ebx, 0(%eax)\n"
    "    movl %esi, 4(%eax)\n"
    "    movl %edi, 8(%eax)\n"
    "    movl %ebp, 12(%eax)\n"
    "    leal 4(%esp), %ecx\n"
    "    movl %ecx, 16(%eax)\n"
    "    movl (%esp), %ecx\n"
    "    movl %ecx, 20(%eax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".global longjmp\n"
    ".type longjmp, @function\n"
    "longjmp:\n"
    "    movl 4(%esp), %edx\n"
    "    movl 8(%esp), %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:  movl 0(%edx), %ebx\n"
    "    movl 4(%edx), %esi\n"
    "    movl 8(%edx), %edi\n"
    "    movl 12(%edx), %ebp\n"
    "    movl 16(%edx), %esp\n"
    "    jmp *20(%edx)\n");

/* musl's libm asks for the rounding mode and raises "inexact": in software floating point there is only
 * round-to-nearest and no exception flags */
int __fe_getround(void) { return 0; }
int __fe_raise_inexact(void) { return 0; }

/* ------------------------------------------------------------------ more of <string.h> */

char *strndup(const char *s, size_t n)
{
    size_t l = 0;
    while (l < n && s[l]) l++;
    char *d = tf_malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}

char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strncat(char *d, const char *s, size_t n)
{
    char *o = d + strlen(d);
    while (n-- && *s) *o++ = *s++;
    *o = 0;
    return d;
}

size_t strspn(const char *s, const char *a)
{
    size_t n = 0;
    while (s[n] && strchr(a, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *r)
{
    size_t n = 0;
    while (s[n] && !strchr(r, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *a)
{
    for (; *s; s++) if (strchr(a, *s)) return (char *)s;
    return 0;
}

size_t strnlen(const char *s, size_t n)
{
    size_t l = 0;
    while (l < n && s[l]) l++;
    return l;
}

size_t tf_usable(const void *p);
size_t malloc_usable_size(void *p) { return tf_usable(p); }

/* ------------------------------------------------------------------ numbers from text */


static unsigned long long parse_u(const char *s, char **end, int base, int *neg, int *ok)
{
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    *neg = 0;
    if (*p == '-' || *p == '+') { *neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_of(p[2]) < 16) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    const char *d0 = p;
    for (; digit_of(*p) < base; p++) v = v * (unsigned)base + (unsigned)digit_of(*p);
    *ok = p > d0;
    if (end) *end = (char *)(*ok ? p : s);
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, ok;
    unsigned long long v = parse_u(s, end, base, &neg, &ok);
    return neg ? -(long long)v : (long long)v;
}
unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, ok;
    unsigned long long v = parse_u(s, end, base, &neg, &ok);
    return neg ? -v : v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long long llabs(long long v) { return v < 0 ? -v : v; }

double pow(double, double);
double strtod(const char *s, char **end)
{
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    double v = 0;
    int any = 0, ex = 0;
    for (; *p >= '0' && *p <= '9'; p++, any = 1) v = v * 10 + (*p - '0');
    if (*p == '.') for (p++; *p >= '0' && *p <= '9'; p++, any = 1) { v = v * 10 + (*p - '0'); ex--; }
    if (!any) { if (end) *end = (char *)s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int en = 0, e = 0;
        if (*q == '-' || *q == '+') { en = *q == '-'; q++; }
        if (*q >= '0' && *q <= '9') {
            for (; *q >= '0' && *q <= '9'; q++) if (e < 10000) e = e * 10 + (*q - '0');
            ex += en ? -e : e;
            p = q;
        }
    }
    if (ex) v *= pow(10, ex);
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ sorting */

static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    /* a heap sort: no recursion, no extra memory */
    char *b = base;
    if (n < 2) return;
    for (size_t start = n / 2; start-- > 0;) {
        size_t root = start;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= n) break;
            if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
            if (cmp(b + root * size, b + child * size) >= 0) break;
            swap_bytes(b + root * size, b + child * size, size);
            root = child;
        }
    }
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        size_t root = 0;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= end) break;
            if (child + 1 < end && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
            if (cmp(b + root * size, b + child * size) >= 0) break;
            swap_bytes(b + root * size, b + child * size, size);
            root = child;
        }
    }
}

/* ------------------------------------------------------------------ time */

struct timeval { long tv_sec; long tv_usec; };
extern uint64_t tfire_now_ms(void);           /* (engine.c: milliseconds since 1970, in integers) */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    uint64_t ms = tfire_now_ms();
    tv->tv_sec = (long)(ms / 1000);
    tv->tv_usec = (long)(ms % 1000) * 1000;
    return 0;
}

double rint(double x);
long lrint(double x) { return (long)rint(x); }

/* ------------------------------------------------------------------ printf, with 64-bit integers and doubles */

typedef struct { char *buf; size_t cap, len; } out_t;

static void out_c(out_t *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(out_t *o, int n, char c) { while (n-- > 0) out_c(o, c); }

/* a finished field: sign/prefix, then the digits, padded to width */
static void out_field(out_t *o, const char *pre, const char *s, int sl, int width, int left, int zero)
{
    int pl = (int)strlen(pre);
    int pad = width - pl - sl;
    if (!left && !zero) out_pad(o, pad, ' ');
    for (int i = 0; i < pl; i++) out_c(o, pre[i]);
    if (!left && zero) out_pad(o, pad, '0');
    for (int i = 0; i < sl; i++) out_c(o, s[i]);
    if (left) out_pad(o, pad, ' ');
}

static int utoa_(unsigned long long v, int base, int upper, char *out)
{
    char tmp[72];
    int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dg[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* the decimal digits of x (>= 0) with prec digits after the point ('f') or in scientific form ('e') */
static int dtoa_(double x, int prec, char kind, char *out, int alt)
{
    int n = 0;
    if (prec > 40) prec = 40;
    int ex = 0;
    if (kind == 'e' && x != 0) {
        while (x >= 10) { x /= 10; ex++; }
        while (x < 1) { x *= 10; ex--; }
    }
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10;
    x += r;
    if (kind == 'e' && x >= 10) { x /= 10; ex++; }
    /* the whole part, digit by digit from the top */
    double p = 1;
    int digits = 1;
    while (p * 10 <= x && digits < 310) { p *= 10; digits++; }
    for (int i = 0; i < digits; i++) {
        int d = (int)(x / p);
        if (d > 9) d = 9;
        if (d < 0) d = 0;
        out[n++] = (char)('0' + d);
        x -= d * p;
        p /= 10;
        if (n > 330) break;
    }
    if (prec || alt) out[n++] = '.';
    for (int i = 0; i < prec; i++) {
        x *= 10;
        int d = (int)x;
        if (d > 9) d = 9;
        if (d < 0) d = 0;
        out[n++] = (char)('0' + d);
        x -= d;
    }
    if (kind == 'e') {
        out[n++] = 'e';
        out[n++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        if (ex < 10) out[n++] = '0';
        n += utoa_((unsigned)ex, 10, 0, out + n);
    }
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    out_t o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_c(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                                  /* 1 long, 2 long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'h') lng--;
            else if (*fmt == 'z' || *fmt == 't') lng = 1;
            else if (*fmt == 'j' || *fmt == 'q' || *fmt == 'L') lng = 2;
            else break;
        }
        char tmp[400];
        int tl;
        char c = *fmt;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            const char *pre = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            tl = utoa_(u, 10, 0, tmp);
            if (prec >= 0) { zero = 0; while (tl < prec) { memmove(tmp + 1, tmp, (size_t)tl); tmp[0] = '0'; tl++; } if (!prec && !u) tl = 0; }
            out_field(&o, pre, tmp, tl, width, left, zero);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long u = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (lng == -1) u = (unsigned short)u;
            if (lng <= -2) u = (unsigned char)u;
            int base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            tl = utoa_(u, base, c == 'X', tmp);
            if (prec >= 0) { zero = 0; while (tl < prec) { memmove(tmp + 1, tmp, (size_t)tl); tmp[0] = '0'; tl++; } if (!prec && !u) tl = 0; }
            out_field(&o, alt && u && base == 16 ? (c == 'X' ? "0X" : "0x") : alt && base == 8 ? "0" : "", tmp, tl, width, left, zero);
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            tl = utoa_(u, 16, 0, tmp);
            out_field(&o, "0x", tmp, tl, width, left, 0);
            break;
        }
        case 'c': tmp[0] = (char)va_arg(ap, int); out_field(&o, "", tmp, 1, width, left, 0); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int sl = 0;
            while (s[sl] && (prec < 0 || sl < prec)) sl++;
            out_field(&o, "", s, sl, width, left, 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': {
            double v = va_arg(ap, double);
            const char *pre = signbit(v) ? "-" : plus ? "+" : space ? " " : "";
            if (signbit(v)) v = -v;
            if (isnan(v)) { out_field(&o, "", "nan", 3, width, left, 0); break; }
            if (isinf(v)) { out_field(&o, pre, "inf", 3, width, left, 0); break; }
            if (prec < 0) prec = 6;
            char k = (c == 'e' || c == 'E') ? 'e' : 'f';
            if (c == 'g' || c == 'G' || c == 'a') {
                /* %g: scientific when the exponent is below -4 or not less than the precision; no trailing zeros */
                if (!prec) prec = 1;
                int ex = 0;
                double t = v;
                if (t != 0) { while (t >= 10) { t /= 10; ex++; } while (t < 1) { t *= 10; ex--; } }
                if (ex < -4 || ex >= prec) { k = 'e'; prec = prec - 1; }
                else { k = 'f'; prec = prec - 1 - ex; }
                tl = dtoa_(v, prec, k, tmp, alt);
                if (!alt) {
                    int epos = tl;
                    for (int i = 0; i < tl; i++) if (tmp[i] == 'e') { epos = i; break; }
                    int dot = -1;
                    for (int i = 0; i < epos; i++) if (tmp[i] == '.') dot = i;
                    if (dot >= 0) {
                        int z = epos;
                        while (z > dot + 1 && tmp[z - 1] == '0') z--;
                        if (z == dot + 1) z = dot;
                        memmove(tmp + z, tmp + epos, (size_t)(tl - epos));
                        tl -= epos - z;
                    }
                }
            } else tl = dtoa_(v, prec, k, tmp, alt);
            if (c == 'E' || c == 'G') for (int i = 0; i < tl; i++) if (tmp[i] == 'e') tmp[i] = 'E';
            out_field(&o, pre, tmp, tl, width, left, zero);
            break;
        }
        case '%': out_c(&o, '%'); break;
        case 'n': break;
        default: out_c(&o, '%'); if (c) out_c(&o, c); else fmt--; break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* printing goes to the page's console (engine.c), a line at a time */
extern void tfire_console_write(const char *s, int len);
typedef struct { int fd; int flags; int eof; int error; } tf_FILE;

int vfprintf(void *fp, const char *fmt, va_list ap)
{
    (void)fp;
    char b[512];
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    tfire_console_write(b, n < (int)sizeof(b) ? n : (int)sizeof(b) - 1);
    return n;
}

int fprintf(void *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(0, fmt, ap);
    va_end(ap);
    return r;
}

int fputc(int c, void *fp) { (void)fp; char ch = (char)c; tfire_console_write(&ch, 1); return c; }
int fputs(const char *s, void *fp) { (void)fp; tfire_console_write(s, (int)strlen(s)); return 0; }

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 1u << 20, fmt, ap);
    va_end(ap);
    return r;
}

/* what GCC calls for __builtin_popcount and __builtin_ctzll on i386 (WebAssembly's popcnt and ctz, in wasm3) */
int __popcountsi2(unsigned a) { int n = 0; while (a) { a &= a - 1; n++; } return n; }
int __popcountdi2(unsigned long long a) { return __popcountsi2((unsigned)a) + __popcountsi2((unsigned)(a >> 32)); }
int __ctzdi2(unsigned long long a) { unsigned lo = (unsigned)a; return lo ? __builtin_ctz(lo) : 32 + __builtin_ctz((unsigned)(a >> 32)); }
