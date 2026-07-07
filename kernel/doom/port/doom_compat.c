/* Implements the extra libc functions declared in this directory's
 * string.h/stdlib.h/math.h/sys/stat.h wrappers -- see those files for
 * why each one is needed. Not part of id Software/doomgeneric's
 * source, unlike everything else under kernel/doom/. */
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include "memory.h"
#include "string.h"
#include "vfs.h"

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == (char)c) last = s;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* DOOM checks a handful of environment variables (DOOMWADPATH,
 * DOOMWADDIR, HOME, ...) purely as optional extra search locations --
 * always reporting "not set" just means it falls back to whatever
 * -iwad path the tOS doom command passes explicitly, not a real gap.
 * kernel/wolf3d/wl_menu.cpp's CheckForEpisodes() is different: unlike
 * DOOM it hard Quit()s if $HOME is unset (it needs somewhere to save
 * config/savegames), so HOME specifically gets a real answer here --
 * the same directory cmd_wolf3d() (kernel/wolf3d/port/wolf_main.cpp)
 * already vfs_chdir()s into for the data files. */
char *getenv(const char *name)
{
    if (name && strcmp(name, "HOME") == 0) return "/assets/wolf3d";
    return NULL;
}

/* Only reachable from a debug/dev code path (checking for the
 * "zenity" binary to show a native error dialog) that requires a real
 * shell to exist -- always "not available" is the correct answer
 * here, not a stubbed-out approximation. */
int system(const char *command)
{
    (void)command;
    return -1;
}

double atof(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') { s++; }
    double whole = 0.0;
    while (*s >= '0' && *s <= '9') { whole = whole * 10.0 + (*s - '0'); s++; }
    double frac = 0.0, scale = 1.0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { frac = frac * 10.0 + (*s - '0'); scale *= 10.0; s++; }
    }
    double result = whole + frac / scale;
    return neg ? -result : result;
}

/* Delegates to kernel/micropython/ports/tos/math_stubs.c's
 * already-tested float implementations rather than duplicating
 * trig/abs logic just for the two double-precision call sites DOOM's
 * generic source actually has (see math.h in this directory). */
float atanf(float x);
float fabsf(float x);

double atan(double x) { return (double)atanf((float)x); }
double fabs(double x) { return (double)fabsf((float)x); }

/* M_MakeDirectory()'s save-game/config directory is a nicety DOOM
 * falls back gracefully without (it just means settings/saves don't
 * persist across runs) -- not worth wiring up a real tOS VFS mkdir
 * for yet. */
int mkdir(const char *path, unsigned mode)
{
    (void)path;
    (void)mode;
    return 0;
}

/* Save-game rewrite (write to a temp file, then swap it into place)
 * -- the real tOS VFS primitives already exist for both, so this is
 * a straightforward bridge rather than a stub. */
int remove(const char *path)
{
    return vfs_unlink(path) == 0 ? 0 : -1;
}

int rename(const char *oldpath, const char *newpath)
{
    return vfs_rename(oldpath, newpath) == 0 ? 0 : -1;
}

/* Only I_Error()'s diagnostic path needs this (a va_list-taking
 * fprintf); built on the vsnprintf+fwrite kernel/lib/stdio.h already
 * has rather than a from-scratch format-string implementation. */
int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;

    /* stdout/stderr aren't backed by a real VFS file descriptor (see
     * kernel/lib/stdio.c: they're just fd=1/fd=2 placeholders) --
     * fprintf() already knows to special-case them and go through
     * putchar() instead of fwrite()'s vfs_write(fp->fd, ...), which
     * silently no-ops on those fds. I_Error()'s call to
     * vfprintf(stderr, ...) needs the same treatment, or its error
     * message vanishes right before exit() (found by DOOM's own
     * startup failing silently instead of explaining why). */
    if (fp == stdout || fp == stderr) {
        for (size_t i = 0; i < len; i++) putchar(buf[i]);
        return (int)len;
    }
    return (int)fwrite(buf, 1, len, fp);
}

/* Only ever called with a handful of exact numeric-conversion
 * patterns (see m_config.c/m_misc.c: "%x", "%i", " 0x%x", " 0X%x",
 * " 0%o", " %d") each parsing a single int out -- a real general
 * sscanf isn't needed, this walks fmt matching literal characters and
 * parses exactly one %x/%o/%d/%i/%u/%c integer conversion. */
int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int conversions = 0;

    while (*fmt) {
        if (*fmt == ' ') {
            while (*str == ' ' || *str == '\t') str++;
            fmt++;
            continue;
        }
        if (*fmt == '%') {
            fmt++;
            char spec = *fmt++;
            int base = (spec == 'x' || spec == 'X') ? 16 : (spec == 'o') ? 8 : 10;
            while (*str == ' ' || *str == '\t') str++;
            const char *start = str;
            int neg = 0;
            if (*str == '-') { neg = 1; str++; }
            long val = 0;
            int any = 0;
            while (*str) {
                int d;
                if (*str >= '0' && *str <= '9') d = *str - '0';
                else if (base == 16 && *str >= 'a' && *str <= 'f') d = *str - 'a' + 10;
                else if (base == 16 && *str >= 'A' && *str <= 'F') d = *str - 'A' + 10;
                else break;
                if (d >= base) break;
                val = val * base + d;
                any = 1;
                str++;
            }
            if (!any) { str = start; break; }
            if (neg) val = -val;
            int *out = va_arg(ap, int *);
            *out = (int)val;
            conversions++;
            continue;
        }
        if (*str != *fmt) break;
        str++;
        fmt++;
    }

    va_end(ap);
    return conversions;
}
