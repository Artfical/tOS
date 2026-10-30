#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <poll.h>
#include <stdlib.h>
static struct termios old;
static void out(const char *s) { write(1, s, strlen(s)); }
static void restore(void) { tcsetattr(0, TCSANOW, &old); out("\033[?25h\033[?1049l\033[0m"); }
int main(void)
{
    tcgetattr(0, &old);
    struct termios t = old; cfmakeraw(&t); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    atexit(restore);
    out("\033[?1049h\033[?25l\033[2J\033[H");
    out("\033[1;37;44m vtdemo: xterm on tOS \033[0m  (q quits, other keys are decoded)\r\n\r\n");
    for (int i = 0; i < 8; i++) { char b[40]; snprintf(b, sizeof b, "\033[%dm  %d  \033[0m", 40 + i, i); out(b); }
    out("\r\n");
    for (int i = 0; i < 8; i++) { char b[40]; snprintf(b, sizeof b, "\033[%dm  %d  \033[0m", 100 + i, i + 8); out(b); }
    out("\r\n\r\n\033[38;2;255;128;0mtrue-colour orange\033[0m  \033[38;5;82m256-colour green\033[0m  \033[1mbold\033[0m  \033[7mreverse\033[0m\r\n");
    out("\r\n\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae\r\n"
        "\xe2\x94\x82 UTF-8  \xe2\x94\x82 caf\xc3\xa9 \xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91\r\n"
        "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf\r\n");
    out("\033[18;1H\033[6n");
    char r[32]; int n = 0;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 1000) > 0) n = read(0, r, sizeof r - 1);
    r[n > 0 ? n : 0] = 0;
    char b[100];
    snprintf(b, sizeof b, "\033[18;1Hcursor report: ");
    out(b);
    for (int i = 0; i < n; i++) { char c[8]; snprintf(c, sizeof c, r[i] == 27 ? "ESC" : "%c", r[i]); out(c); }
    out("\r\n");
    int row = 20;
    for (;;) {
        char k[16];
        int m = read(0, k, sizeof k);
        if (m <= 0) break;
        if (m == 1 && k[0] == 'q') break;
        snprintf(b, sizeof b, "\033[%d;1H\033[Kkey:", row);
        out(b);
        for (int i = 0; i < m; i++) { snprintf(b, sizeof b, " %02x", (unsigned char)k[i]); out(b); }
        if (++row > 23) row = 20;
    }
    return 0;
}
