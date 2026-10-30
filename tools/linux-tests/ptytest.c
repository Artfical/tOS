/* Pseudo-terminals the way a login server uses them: openpty-style setup through glibc, a child with the
 * slave as its terminal (setsid, TIOCSCTTY, dup2), cooked lines with echo, the window size, ^C. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static volatile int got_int;
static void on_int(int s) { (void)s; got_int = 1; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("ptytest: FAIL %s\n", what); fails++; } else printf("ptytest: ok %s\n", what); } while (0)

/* everything the master has given so far; read until `want` has shown up (or a few seconds pass) */
static char all[4096];
static int alln;
static int read_until(int m, const char *want)
{
    for (int tries = 0; tries < 60; tries++) {
        all[alln] = 0;
        if (strstr(all, want)) return 1;
        struct pollfd p = { m, POLLIN, 0 };
        if (poll(&p, 1, 500) > 0) {
            int k = read(m, all + alln, (int)sizeof(all) - 1 - alln);
            if (k > 0) alln += k;
            if (k < 0 && errno != EAGAIN) break;
        }
    }
    all[alln] = 0;
    return strstr(all, want) != 0;
}

int main(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0, "posix_openpt");
    CHECK(grantpt(m) == 0, "grantpt");
    CHECK(unlockpt(m) == 0, "unlockpt");
    char *sn = ptsname(m);
    CHECK(sn && !strncmp(sn, "/dev/pts/", 9), "ptsname");
    if (!sn) return 1;
    struct winsize ws = { 30, 100, 0, 0 };
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ");

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(sn, O_RDWR);
        if (s < 0) { printf("child: open %s failed: %d\n", sn, errno); _exit(2); }
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        struct winsize w;
        ioctl(0, TIOCGWINSZ, &w);
        printf("child: isatty=%d size=%dx%d\n", isatty(0), w.ws_row, w.ws_col);
        fflush(stdout);
        char line[100];
        if (fgets(line, sizeof(line), stdin)) printf("child got: %s", line);
        fflush(stdout);
        signal(SIGINT, on_int);
        printf("child: waiting\n");
        fflush(stdout);
        char c;
        int r = read(0, &c, 1);
        printf("child: read=%d sigint=%d\n", r, got_int);
        fflush(stdout);
        _exit(got_int ? 7 : 3);
    }
    close(open("/dev/null", O_RDONLY));
    char buf[2048];
    CHECK(read_until(m, "size=30x100"), "the child sees the window size and a terminal");
    write(m, "hello\n", 6);
    CHECK(read_until(m, "child got: hello"), "a cooked line reaches the child");
    CHECK(strstr(all, "hello\r\n") != 0, "the line was echoed with \\r\\n");
    CHECK(read_until(m, "child: waiting"), "the child waits");
    write(m, "\003", 1);
    CHECK(read_until(m, "sigint=1"), "^C becomes SIGINT for the child");
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7, "the child ended with 7");
    int k = read(m, buf, sizeof(buf));
    CHECK(k < 0 && errno == EIO, "the master reads EIO once the child is gone");
    printf(fails ? "ptytest: %d FAILED\n" : "ptytest: all passed\n", fails);
    return fails ? 1 : 0;
}
