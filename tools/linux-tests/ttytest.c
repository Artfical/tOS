#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <string.h>
static int bad;
#define CK(c, m) do { if (c) printf("[TTY] ok   %s\n", m); else { printf("[TTY] FAIL %s (errno %d)\n", m, errno); bad++; } } while (0)
static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    CK(isatty(0) && isatty(1) && isatty(2), "isatty 0 1 2");
    struct termios t0, t;
    CK(tcgetattr(0, &t0) == 0, "tcgetattr");
    CK((t0.c_lflag & ICANON) && (t0.c_lflag & ECHO) && (t0.c_oflag & OPOST), "cooked by default");
    struct winsize ws;
    CK(ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row == 23 && ws.ws_col == 80, "window size 23x80");
    t = t0; cfmakeraw(&t);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 2;
    CK(tcsetattr(0, TCSANOW, &t) == 0, "tcsetattr raw");
    struct termios r;
    tcgetattr(0, &r);
    CK(!(r.c_lflag & (ICANON | ECHO | ISIG)) && !(r.c_oflag & OPOST) && r.c_cc[VMIN] == 0 && r.c_cc[VTIME] == 2, "raw flags kept");
    char b[8];
    long a = ms();
    int n = read(0, b, sizeof b);
    long el = ms() - a;
    CK(n == 0 && el >= 150 && el < 1500, "VMIN=0 VTIME=2 read times out");
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    a = ms();
    n = read(0, b, sizeof b);
    CK(n == 0 && ms() - a < 500, "VMIN=0 VTIME=0 read returns at once");
    t.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &t);
    int fl = fcntl(0, F_GETFL);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);
    CK((fcntl(0, F_GETFL) & O_NONBLOCK) != 0, "O_NONBLOCK sticks");
    n = read(0, b, sizeof b);
    CK(n < 0 && errno == EAGAIN, "nonblocking read is EAGAIN");
    struct pollfd p = { 0, POLLIN, 0 };
    a = ms();
    n = poll(&p, 1, 200);
    CK(n == 0 && ms() - a >= 150, "poll(stdin) waits and finds nothing");
    int ready = 0; ioctl(0, FIONREAD, &ready);
    CK(ready == 0, "FIONREAD 0");
    fcntl(0, F_SETFL, fl);
    CK(tcsetattr(0, TCSANOW, &t0) == 0, "tcsetattr back");
    tcgetattr(0, &r);
    CK(r.c_lflag == t0.c_lflag && r.c_iflag == t0.c_iflag, "cooked again");
    pid_t g = tcgetpgrp(0);
    CK(g > 0, "tcgetpgrp");
    printf("[TTY] %s\n", bad ? "FAILED" : "all ok");
    return bad;
}
