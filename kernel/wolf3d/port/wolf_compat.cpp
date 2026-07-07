/* tOS compat glue for the vendored Wolf4SDL engine (kernel/wolf3d/)
 * -- not part of Wolf4SDL/id Software's source. Implements the one
 * genuinely new libc gap kernel/wolf3d/port/stdlib.h declares:
 * atexit(). Nothing else in tOS has needed real libc atexit()
 * semantics before now (DOOM has its own separate I_AtExit() list,
 * unrelated to this). tOS's own exit()/abort() (kernel/lib/stdlib.c)
 * just spin forever rather than actually unwinding, so these
 * registered functions are never invoked by a real exit() call --
 * kernel/wolf3d/port/wolf_main.cpp's shell command instead calls
 * wolf_run_atexit_handlers() directly on Ctrl+C, mirroring how DOOM's
 * platform layer handles its own equivalent shutdown step.
 */
#include "stdlib.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "math.h"
#include "wolf_compat.h"
extern "C" {
#include "vfs.h"
}
#include <stdarg.h>

/* Double-precision wrappers around kernel/micropython/ports/tos/
 * math_stubs.c's float versions -- same delegation pattern
 * kernel/doom/port/doom_compat.c already uses for its own two.
 * atan()/fabs() specifically are NOT redefined here even though
 * kernel/wolf3d's source needs them too: doom_compat.c already
 * provides both as plain (C-linkage) global symbols linked into the
 * same final kernel/tOS.elf, so doing it again here would be a
 * duplicate-symbol error (see kernel/wolf3d/port/stdlib.h for the
 * same reasoning applied to getenv()/system()/atof()). */
extern "C" {
float sinf(float x);
float tanf(float x);
float atan2f(float y, float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float powf(float x, float y);
}

double sin(double x) { return (double)sinf((float)x); }
double cos(double x) { return (double)sinf((float)x + 1.5707963f); }
double tan(double x) { return (double)tanf((float)x); }
double atan2(double y, double x) { return (double)atan2f((float)y, (float)x); }
double sqrt(double x) { return (double)sqrtf((float)x); }
double floor(double x) { return (double)floorf((float)x); }
double ceil(double x) { return (double)ceilf((float)x); }
double pow(double x, double y) { return (double)powf((float)x, (float)y); }

extern "C" int stat(const char *path, struct stat *buf)
{
    (void)buf;
    vfs_entry_t entry;
    return vfs_stat(path, &entry);
}

/* open()/read()/write()/close()/lseek()/unlink() -- bridged onto
 * kernel/fs/vfs.h's vfs_*() calls, the same VFS every other
 * filesystem access in tOS already goes through (see
 * kernel/wolf3d/port/fcntl.h's comment). O_* flag values here were
 * deliberately chosen to already match VFS_*'s values 1:1 (both are
 * 0/1/2/0x100/0x200/0x400 for RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND),
 * so no translation table is needed, just a straight pass-through. */
extern "C" int open(const char *path, int flags, ...)
{
    return vfs_open(path, flags);
}

extern "C" int close(int fd)
{
    return vfs_close(fd);
}

extern "C" int read(int fd, void *buf, size_t count)
{
    return vfs_read(fd, buf, (uint32_t)count);
}

extern "C" int write(int fd, const void *buf, size_t count)
{
    return vfs_write(fd, buf, (uint32_t)count);
}

extern "C" long lseek(int fd, long offset, int whence)
{
    return vfs_lseek(fd, (uint32_t)offset, whence);
}

extern "C" int unlink(const char *path)
{
    return vfs_unlink(path);
}

#define MAX_ATEXIT_HANDLERS 8
static void (*g_atexit_handlers[MAX_ATEXIT_HANDLERS])(void);
static int g_atexit_count = 0;

extern "C" int atexit(void (*func)(void))
{
    if (g_atexit_count >= MAX_ATEXIT_HANDLERS) return -1;
    g_atexit_handlers[g_atexit_count++] = func;
    return 0;
}

void wolf_run_atexit_handlers(void)
{
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_handlers[i]) g_atexit_handlers[i]();
    }
    g_atexit_count = 0;
}
