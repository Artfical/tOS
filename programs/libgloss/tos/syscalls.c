#include "syscalls.h"

static inline long syscall(long n, long a, long b, long c, long d)
{
    long ret;
    asm volatile("int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory");
    return ret;
}

void _exit(int status)
{
    syscall(SYS_EXIT, status, 0, 0, 0);
    for (;;);
}

int _close(int fd)
{
    return syscall(SYS_CLOSE, fd, 0, 0, 0);
}

int _execve(const char *path, char *const argv[], char *const envp[])
{
    return syscall(SYS_EXECVE, (long)path, (long)argv, (long)envp, 0);
}

int _fork(void)
{
    return syscall(SYS_FORK, 0, 0, 0, 0);
}

int _fstat(int fd, struct stat *st)
{
    return syscall(SYS_FSTAT, fd, (long)st, 0, 0);
}

int _getpid(void)
{
    return syscall(SYS_GETPID, 0, 0, 0, 0);
}

int _isatty(int fd)
{
    return syscall(SYS_ISATTY, fd, 0, 0, 0);
}

int _kill(int pid, int sig)
{
    return syscall(SYS_KILL, pid, sig, 0, 0);
}

int _link(const char *old, const char *new)
{
    return -1;
}

int _lseek(int fd, int offset, int whence)
{
    return syscall(SYS_LSEEK, fd, offset, whence, 0);
}

int _open(const char *path, int flags, int mode)
{
    return syscall(SYS_OPEN, (long)path, flags, 0, 0);
}

int _read(int fd, char *buf, int count)
{
    return syscall(SYS_READ, fd, (long)buf, count, 0);
}

caddr_t _sbrk(int incr)
{
    static char *heap_end = 0;
    char *prev;
    if (heap_end == 0)
        heap_end = (char *)syscall(SYS_BRK, 0, 0, 0, 0);
    prev = heap_end;
    if (syscall(SYS_BRK, (long)(heap_end + incr), 0, 0, 0) < 0)
        return (caddr_t)-1;
    heap_end += incr;
    return (caddr_t)prev;
}

int _stat(const char *path, struct stat *st)
{
    int fd = _open(path, 0, 0);
    if (fd < 0) return -1;
    int ret = _fstat(fd, st);
    _close(fd);
    return ret;
}

int _times(struct tms *buf)
{
    return -1;
}

int _unlink(const char *path)
{
    return -1;
}

int _wait(int *status)
{
    return syscall(SYS_WAITPID, 0, 0, 0, 0);
}

int _write(int fd, const char *buf, int count)
{
    return syscall(SYS_WRITE, fd, (long)buf, count, 0);
}
