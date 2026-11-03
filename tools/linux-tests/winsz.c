/* the terminal's size, and what happens when its window is resized or the mouse is used: winsz [mouse] */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
static volatile int changed;
static void on_winch(int s) { (void)s; changed = 1; }
static void show(void)
{
    struct winsize w;
    if (ioctl(0, TIOCGWINSZ, &w) == 0) printf("size %d rows x %d cols\r\n", w.ws_row, w.ws_col);
    else printf("TIOCGWINSZ failed\r\n");
    fflush(stdout);
}
int main(int argc, char **argv)
{
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_winch; sigaction(SIGWINCH, &sa, NULL);
    show();
    int mouse = argc > 1 && !strcmp(argv[1], "mouse");
    struct termios t, old; tcgetattr(0, &old); t = old;
    cfmakeraw(&t); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 2;
    if (mouse) { tcsetattr(0, TCSANOW, &t); printf("\x1b[?1000h\x1b[?1006h"); fflush(stdout); }
    for (int i = 0; i < 600; i++) {
        if (changed) { changed = 0; show(); }
        if (mouse) {
            char b[64]; int n = read(0, b, sizeof(b) - 1);
            if (n > 0) { b[n] = 0; for (int k = 0; k < n; k++) if (b[k] == 27) b[k] = '^'; printf("input: %s\r\n", b); fflush(stdout); if (b[0] == 'q') break; }
        } else usleep(100000);
    }
    if (mouse) { printf("\x1b[?1000l\x1b[?1006l"); tcsetattr(0, TCSANOW, &old); }
    return 0;
}
