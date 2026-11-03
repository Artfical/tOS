/* the *at calls with a directory descriptor (Python's shutil.rmtree, find, rm -r use them) */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("attest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("attest: ok %s\n", what); } while (0)
int main(void)
{
    mkdir("/tmp/ad", 0755); mkdir("/tmp/other", 0755);
    chdir("/tmp/other");
    int d = open("/tmp/ad", O_RDONLY | O_DIRECTORY);
    CHECK(d >= 0, "open a directory");
    CHECK(mkdirat(d, "sub", 0755) == 0, "mkdirat in that directory");
    struct stat st;
    CHECK(stat("/tmp/ad/sub", &st) == 0 && S_ISDIR(st.st_mode), "it is /tmp/ad/sub, not in the working directory");
    CHECK(stat("/tmp/other/sub", &st) == -1, "and not in /tmp/other");
    int f = openat(d, "file.txt", O_CREAT | O_WRONLY, 0644); write(f, "hi", 2); close(f);
    CHECK(stat("/tmp/ad/file.txt", &st) == 0 && st.st_size == 2, "openat creates in the directory");
    CHECK(fstatat(d, "file.txt", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode), "fstatat relative to it");
    CHECK(faccessat(d, "file.txt", R_OK, 0) == 0, "faccessat");
    CHECK(renameat(d, "file.txt", d, "moved.txt") == 0 && stat("/tmp/ad/moved.txt", &st) == 0, "renameat");
    char cwd[64]; getcwd(cwd, sizeof(cwd));
    CHECK(!strcmp(cwd, "/tmp/other"), "the working directory is unchanged");
    CHECK(unlinkat(d, "moved.txt", 0) == 0 && stat("/tmp/ad/moved.txt", &st) == -1, "unlinkat a file");
    CHECK(unlinkat(d, "sub", AT_REMOVEDIR) == 0 && stat("/tmp/ad/sub", &st) == -1, "unlinkat AT_REMOVEDIR");
    CHECK(unlinkat(d, "nothing", 0) == -1 && errno == ENOENT, "unlinkat of a missing name: ENOENT");
    { DIR *dd = opendir("/tmp/ad"); struct dirent *de; while (dd && (de = readdir(dd))) printf("attest: left in /tmp/ad: %s\n", de->d_name); if (dd) closedir(dd); }
    close(d);
    CHECK(unlinkat(-100, "/tmp/ad", AT_REMOVEDIR) == 0, "AT_FDCWD with an absolute path");
    rmdir("/tmp/other");
    printf(fails ? "attest: %d FAILED\n" : "attest: all passed\n", fails);
    return fails != 0;
}
