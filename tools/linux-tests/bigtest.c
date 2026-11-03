/* offsets past what the file system holds fail cleanly (they used to wrap to a wrong place) */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("bigtest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("bigtest: ok %s\n", what); } while (0)
int main(void)
{
    int fd = open("/tmp/bigtest.dat", O_CREAT | O_TRUNC | O_RDWR, 0600);
    write(fd, "0123456789", 10);
    CHECK(lseek(fd, 5LL << 30, SEEK_SET) == -1 && errno == EOVERFLOW, "lseek to 5 GB: EOVERFLOW");
    CHECK(lseek(fd, 3, SEEK_SET) == 3, "a small lseek still works");
    char c; CHECK(read(fd, &c, 1) == 1 && c == '3', "and reads the right byte (the failed seek moved nothing)");
    CHECK(ftruncate(fd, 5LL << 30) == -1 && errno == EFBIG, "ftruncate to 5 GB: EFBIG");
    CHECK(pwrite(fd, "x", 1, 6LL << 30) == -1 && errno == EFBIG, "pwrite at 6 GB: EFBIG");
    CHECK(pread(fd, &c, 1, 6LL << 30) == -1 && errno == EOVERFLOW, "pread at 6 GB: EOVERFLOW");
    CHECK(pread(fd, &c, 1, 5) == 1 && c == '5', "pread at 5 works");
    close(fd); unlink("/tmp/bigtest.dat");
    printf(fails ? "bigtest: %d FAILED\n" : "bigtest: all passed\n", fails);
    return fails != 0;
}
