/* /proc as programs use it */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("proctest: FAIL %s (errno %d)\n", what, errno); fails++; } else printf("proctest: ok %s\n", what); } while (0)
static int slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0, k;
    while (n < cap - 1 && (k = read(fd, buf + n, cap - 1 - n)) > 0) n += k;
    buf[n] = 0;
    close(fd);
    return n;
}
int main(void)
{
    char b[8192];
    CHECK(slurp("/proc/cpuinfo", b, sizeof(b)) > 50 && strstr(b, "processor") && strstr(b, "model name") && strstr(b, "flags"), "/proc/cpuinfo has processor, model name and flags");
    long mt = 0, mf = 0;
    CHECK(slurp("/proc/meminfo", b, sizeof(b)) > 50, "/proc/meminfo reads");
    char *p = strstr(b, "MemTotal:"); if (p) mt = atol(p + 9);
    p = strstr(b, "MemFree:"); if (p) mf = atol(p + 8);
    CHECK(mt > 100000 && mf > 0 && mf <= mt, "MemTotal and MemFree make sense");
    CHECK(slurp("/proc/version", b, sizeof(b)) > 10 && !strncmp(b, "Linux version", 13), "/proc/version");
    CHECK(slurp("/proc/uptime", b, sizeof(b)) > 3 && atof(b) > 0.5, "/proc/uptime");
    CHECK(slurp("/proc/loadavg", b, sizeof(b)) > 10, "/proc/loadavg");
    CHECK(slurp("/proc/stat", b, sizeof(b)) > 10 && !strncmp(b, "cpu ", 4) && strstr(b, "btime"), "/proc/stat");
    CHECK(slurp("/proc/mounts", b, sizeof(b)) > 5, "/proc/mounts");
    CHECK(slurp("/proc/sys/kernel/osrelease", b, sizeof(b)) > 2, "/proc/sys/kernel/osrelease");
    char st[4096];
    int n = slurp("/proc/self/status", st, sizeof(st));
    char *q = strstr(st, "\nPid:");
    CHECK(n > 40 && q && atoi(q + 5) == (int)getpid(), "/proc/self/status names this process");
    CHECK(strstr(st, "Name:\tproctest") != NULL, "and its name");
    n = slurp("/proc/self/stat", b, sizeof(b));
    CHECK(n > 20 && atoi(b) == (int)getpid() && strstr(b, "(proctest)"), "/proc/self/stat starts with pid (name)");
    char exe[256]; ssize_t el = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (el > 0) exe[el] = 0;
    CHECK(el > 0 && strstr(exe, "proctest"), "readlink /proc/self/exe is this program");
    char link[256]; ssize_t ll = readlink("/proc/self/cwd", link, sizeof(link) - 1);
    CHECK(ll > 0, "readlink /proc/self/cwd");
    /* a folder per process, and the open files of this one */
    DIR *d = opendir("/proc");
    int saw_self = 0, saw_pid = 0, saw_cpu = 0; char me[16]; sprintf(me, "%d", (int)getpid());
    struct dirent *e;
    while (d && (e = readdir(d))) { if (!strcmp(e->d_name, "self")) saw_self = 1; if (!strcmp(e->d_name, me)) saw_pid = 1; if (!strcmp(e->d_name, "cpuinfo")) saw_cpu = 1; }
    if (d) closedir(d);
    CHECK(saw_self && saw_pid && saw_cpu, "readdir /proc lists self, this pid and cpuinfo");
    int extra = open("/proc/version", O_RDONLY), extra2 = open("/tmp", O_RDONLY);
    d = opendir("/proc/self/fd");
    int count = 0, saw_extra = 0; char en[16]; sprintf(en, "%d", extra);
    while (d && (e = readdir(d))) { if (e->d_name[0] >= '0' && e->d_name[0] <= '9') count++; if (!strcmp(e->d_name, en)) saw_extra = 1; }
    if (d) closedir(d);
    CHECK(count >= 4 && saw_extra, "/proc/self/fd lists the open descriptors");
    close(extra); close(extra2);
    struct stat sb;
    CHECK(stat("/proc", &sb) == 0 && S_ISDIR(sb.st_mode), "stat /proc is a folder");
    CHECK(stat("/proc/self", &sb) == 0 && S_ISDIR(sb.st_mode), "stat /proc/self is a folder");
    CHECK(access("/proc/cpuinfo", R_OK) == 0, "access /proc/cpuinfo");
    CHECK(open("/proc/nosuchthing", O_RDONLY) < 0 && errno == ENOENT, "an unknown /proc file is not found");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    CHECK(cpus >= 1, "sysconf online processors");
    printf(fails ? "proctest: %d FAILED\n" : "proctest: all passed\n", fails);
    return fails != 0;
}
