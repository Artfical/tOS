/* the file calls a database engine uses, one by one, with what each returned */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/file.h>
#define P(what, expr) do { errno = 0; long r_ = (long)(expr); printf("probe: %-34s = %ld%s%s\n", what, r_, r_ < 0 ? " errno " : "", r_ < 0 ? strerror(errno) : ""); } while (0)
int main(void)
{
    unlink("/tmp/probe.db"); unlink("/tmp/probe.db-journal");
    int fd; P("open O_RDWR|O_CREAT|O_NOFOLLOW|CLOEXEC", fd = open("/tmp/probe.db", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644));
    char buf[4096]; memset(buf, 'x', sizeof(buf));
    P("pwrite 4096 at 0", pwrite(fd, buf, 4096, 0));
    P("pwrite 4096 at 8192 (a gap)", pwrite(fd, buf, 4096, 8192));
    struct stat st; P("fstat", fstat(fd, &st)); printf("probe: size %ld mode %o\n", (long)st.st_size, st.st_mode);
    char rb[4096]; P("pread 4096 at 8192", pread(fd, rb, 4096, 8192));
    P("pread in the gap (zeros?)", pread(fd, rb, 100, 5000)); printf("probe: gap byte %d\n", rb[0]);
    struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 1073741824, .l_len = 1 };
    P("fcntl F_SETLK read lock", fcntl(fd, F_SETLK, &fl));
    fl.l_type = F_WRLCK; fl.l_start = 1073741825; fl.l_len = 2;
    P("fcntl F_SETLK write lock", fcntl(fd, F_SETLK, &fl));
    fl.l_type = F_WRLCK; P("fcntl F_GETLK", fcntl(fd, F_GETLK, &fl));
    fl.l_type = F_UNLCK; P("fcntl F_SETLK unlock", fcntl(fd, F_SETLK, &fl));
    P("flock LOCK_EX", flock(fd, LOCK_EX)); P("flock LOCK_UN", flock(fd, LOCK_UN));
    P("fsync", fsync(fd)); P("fdatasync", fdatasync(fd));
    P("ftruncate to 4096", ftruncate(fd, 4096)); P("fstat after", fstat(fd, &st)); printf("probe: size %ld\n", (long)st.st_size);
    P("fchmod 0600", fchmod(fd, 0600)); P("fchown", fchown(fd, 0, 0));
    P("lstat path", lstat("/tmp/probe.db", &st)); P("stat dir /tmp", stat("/tmp", &st));
    P("access R_OK|W_OK", access("/tmp/probe.db", R_OK | W_OK));
    char cwd[200]; P("getcwd", getcwd(cwd, sizeof(cwd)) ? 0 : -1);
    int j; P("open journal O_EXCL", j = open("/tmp/probe.db-journal", O_RDWR | O_CREAT | O_EXCL, 0644));
    P("write journal", write(j, buf, 512)); P("lseek journal", lseek(j, 0, SEEK_SET)); P("fsync journal", fsync(j));
    int dfd; P("open dir O_RDONLY", dfd = open("/tmp", O_RDONLY)); P("fsync dir", fsync(dfd)); close(dfd);
    P("unlink journal", unlink("/tmp/probe.db-journal")); close(j);
    void *m; P("mmap file read-only", (m = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED ? -1 : 0);
    P("mmap file private rw", (m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) == MAP_FAILED ? -1 : 0);
    P("mmap anon shared", (m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED ? -1 : 0);
    P("sysconf page size", sysconf(_SC_PAGESIZE));
    int n; P("open 3 times the same file", (n = open("/tmp/probe.db", O_RDONLY)) + (j = open("/tmp/probe.db", O_RDONLY)));
    P("pread via the other fd", pread(n, rb, 10, 0));
    close(n); close(j); close(fd);
    P("unlink", unlink("/tmp/probe.db"));
    return 0;
}
