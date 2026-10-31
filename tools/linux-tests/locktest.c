/* File locks between processes: fcntl byte ranges (conflicts, F_GETLK, wait, unlock, a lock dying with its process) and flock */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/wait.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("locktest: FAIL %s\n", what); fails++; } else printf("locktest: ok %s\n", what); } while (0)
static int rng(int fd, int type, long start, long len, int cmd)
{
    struct flock f = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &f);
}
int main(void)
{
    int fd = open("/tmp/lock.db", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    CHECK(rng(fd, F_WRLCK, 100, 10, F_SETLK) == 0, "write-lock bytes 100..109");
    int p1[2], p2[2];
    pipe(p1); pipe(p2);
    pid_t child = fork();
    if (child == 0) {
        int c = open("/tmp/lock.db", O_RDWR);
        char ok = 0;
        errno = 0; int r = rng(c, F_WRLCK, 105, 2, F_SETLK);
        ok |= (r == -1 && (errno == EAGAIN || errno == EACCES)) ? 1 : 0;           /* overlapping: refused */
        ok |= rng(c, F_WRLCK, 200, 5, F_SETLK) == 0 ? 2 : 0;                       /* elsewhere: fine */
        struct flock g = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 1 };
        ok |= (fcntl(c, F_GETLK, &g) == 0 && g.l_type == F_WRLCK && g.l_pid > 0 && g.l_start == 100) ? 4 : 0;   /* who is in the way */
        struct flock g2 = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 300, .l_len = 1 };
        ok |= (fcntl(c, F_GETLK, &g2) == 0 && g2.l_type == F_UNLCK) ? 8 : 0;      /* nobody there */
        ok |= rng(c, F_RDLCK, 100, 10, F_SETLK) == -1 ? 16 : 0;                    /* a read lock against a write lock */
        write(p1[1], &ok, 1);
        char go; read(p2[0], &go, 1);                                              /* the parent has unlocked */
        ok = rng(c, F_WRLCK, 100, 10, F_SETLKW) == 0 ? 1 : 0;                      /* now it is ours (waits if need be) */
        write(p1[1], &ok, 1);
        _exit(0);                                                                  /* dies holding 100..109 and 200..204 */
    }
    char ok = 0; read(p1[0], &ok, 1);
    CHECK(ok & 1, "an overlapping lock is refused for another process");
    CHECK(ok & 2, "a lock elsewhere in the file is granted");
    CHECK(ok & 4, "F_GETLK names the lock in the way");
    CHECK(ok & 8, "F_GETLK says F_UNLCK where nothing is locked");
    CHECK(ok & 16, "a read lock is refused against a write lock");
    CHECK(rng(fd, F_UNLCK, 100, 10, F_SETLK) == 0, "unlock bytes 100..109");
    write(p2[1], "g", 1);
    read(p1[0], &ok, 1);
    CHECK(ok == 1, "the other process gets the range once it is free");
    int st; waitpid(child, &st, 0);
    CHECK(rng(fd, F_WRLCK, 100, 10, F_SETLK) == 0 && rng(fd, F_WRLCK, 200, 5, F_SETLK) == 0, "a dead process's locks are gone");
    /* shared read locks coexist, an unlock in the middle splits a lock */
    CHECK(rng(fd, F_RDLCK, 0, 0, F_SETLK) == 0, "read-lock the whole file");
    CHECK(rng(fd, F_UNLCK, 50, 10, F_SETLK) == 0, "unlock a hole in the middle");
    pid_t c2 = fork();
    if (c2 == 0) {
        int c = open("/tmp/lock.db", O_RDWR);
        char r = 0;
        r |= rng(c, F_RDLCK, 0, 0, F_SETLK) == 0 ? 1 : 0;                          /* readers share */
        r |= rng(c, F_WRLCK, 50, 10, F_SETLK) == 0 ? 2 : 0;                        /* the hole is free to write */
        r |= rng(c, F_WRLCK, 70, 1, F_SETLK) == -1 ? 4 : 0;                        /* outside it, still read-locked */
        _exit(r);
    }
    waitpid(c2, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "readers share, and the unlocked hole can be written");
    /* flock(2) */
    int f1 = open("/tmp/flock.file", O_RDWR | O_CREAT, 0644);
    CHECK(flock(f1, LOCK_EX) == 0, "flock exclusive");
    pid_t c3 = fork();
    if (c3 == 0) {
        int c = open("/tmp/flock.file", O_RDWR);
        char r = 0;
        r |= (flock(c, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK) ? 1 : 0;
        r |= (flock(c, LOCK_SH | LOCK_NB) == -1) ? 2 : 0;
        _exit(r);
    }
    waitpid(c3, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3, "another process cannot flock it (EWOULDBLOCK)");
    CHECK(flock(f1, LOCK_UN) == 0, "flock unlock");
    pid_t c4 = fork();
    if (c4 == 0) { int c = open("/tmp/flock.file", O_RDWR); _exit(flock(c, LOCK_EX | LOCK_NB) == 0 ? 0 : 1); }
    waitpid(c4, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "after the unlock it can");
    printf(fails ? "locktest: %d FAILED\n" : "locktest: all passed\n", fails);
    return fails != 0;
}
