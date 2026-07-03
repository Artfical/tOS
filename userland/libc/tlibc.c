/* tlibc — minimal freestanding shared C library for tOS user programs.
 * Compiled as a real ELF shared object (libc.so) and dynamically
 * linked against by user executables through tOS's own ELF loader
 * (kernel/fs/elf.c: elf_load_dynamic). Syscall numbers match the
 * classic Linux i386 int 0x80 ABI, which tOS's syscall_handler()
 * implements a subset of. */

typedef unsigned int   size_t;
typedef int            ssize_t;

#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_BRK     17
#define SYS_LSEEK   19
#define SYS_GETPID  20

static inline int syscall1(int n, int a)
{
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a) : "memory");
    return ret;
}

static inline int syscall3(int n, int a, int b, int c)
{
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

void _exit(int code)
{
    syscall1(SYS_EXIT, code);
    for (;;) { }
}

void exit(int code)
{
    _exit(code);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, fd, (int)buf, (int)count);
}

ssize_t read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, fd, (int)buf, (int)count);
}

int open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (int)path, flags, 0);
}

int close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}

int getpid(void)
{
    return syscall1(SYS_GETPID, 0);
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *dst, int val, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)val;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

/* Bump allocator on top of SYS_BRK — enough for simple test programs. */
static char *heap_ptr = 0;
static char *heap_end = 0;

void *malloc(size_t size)
{
    size = (size + 15) & ~15u;
    if (heap_ptr + size > heap_end) {
        int want = (int)(heap_ptr + size);
        int got = syscall1(SYS_BRK, want + 0x10000);
        if (got == 0) return (void *)0;
        heap_end = (char *)got;
        if (heap_ptr == 0) heap_ptr = (char *)(got - 0x10000 - (int)size);
    }
    void *p = heap_ptr;
    heap_ptr += size;
    return p;
}

void free(void *ptr)
{
    (void)ptr; /* bump allocator: no-op */
}

static void itoa10(int v, char *buf)
{
    char tmp[12];
    int n = 0, neg = 0;
    unsigned int u;
    if (v < 0) { neg = 1; u = (unsigned int)(-v); } else { u = (unsigned int)v; }
    if (u == 0) tmp[n++] = '0';
    while (u > 0) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    int k = 0;
    if (neg) buf[k++] = '-';
    while (n > 0) buf[k++] = tmp[--n];
    buf[k] = 0;
}

int printf(const char *fmt, ...)
{
    unsigned int *args = (unsigned int *)&fmt + 1;
    int argi = 0;
    char numbuf[16];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { write(1, p, 1); continue; }
        p++;
        if (*p == 'd') {
            itoa10((int)args[argi++], numbuf);
            write(1, numbuf, strlen(numbuf));
        } else if (*p == 's') {
            const char *s = (const char *)args[argi++];
            write(1, s, strlen(s));
        } else if (*p == '%') {
            write(1, "%", 1);
        }
    }
    return 0;
}
