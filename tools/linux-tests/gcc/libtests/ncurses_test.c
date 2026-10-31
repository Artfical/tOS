#include "common.h"
#include <curses.h>
#include <term.h>
#include <unistd.h>
int main(void)
{
    setenv("TERM", "xterm", 1);
    int err = 0;
    CHECK(setupterm(NULL, 1, &err) == OK && err == 1, "setupterm finds xterm in the terminfo database");
    char *cup = tigetstr("cup");
    CHECK(cup && cup != (char *)-1, "tigetstr cup");
    if (cup && cup != (char *)-1) {
        char *s = tparm(cup, 5, 10, 0, 0, 0, 0, 0, 0, 0);
        CHECK(s && !strcmp(s, "\033[6;11H"), "tparm cup 5,10 = ESC[6;11H");
    }
    CHECK(tigetnum("colors") == 8, "xterm has 8 colours");
    CHECK(tigetflag("am") == 1, "xterm has automatic margins");
    char *bold = tigetstr("bold");
    CHECK(bold && bold != (char *)-1 && !strcmp(bold, "\033[1m"), "bold is ESC[1m");
    setenv("TERM", "xterm-256color", 1);
    CHECK(setupterm(NULL, 1, &err) == OK && tigetnum("colors") == 256, "xterm-256color has 256 colours");
    setenv("TERM", "vt100", 1);
    CHECK(setupterm(NULL, 1, &err) == OK && tigetnum("cols") == 80, "vt100 is 80 columns");
    /* real curses on a screen that goes to a file */
    setenv("TERM", "xterm", 1);
    FILE *out = fopen("/tmp/curses.out", "w+");
    FILE *in = fopen("/dev/null", "r");
    SCREEN *scr = newterm("xterm", out, in);
    CHECK(scr != NULL, "newterm");
    if (scr) {
        set_term(scr);
        cbreak(); noecho();
        start_color();
        init_pair(1, COLOR_RED, COLOR_BLACK);
        attron(COLOR_PAIR(1) | A_BOLD);
        mvaddstr(3, 4, "hello curses");
        attroff(COLOR_PAIR(1) | A_BOLD);
        mvprintw(5, 0, "%d x %d", 12, 34);
        WINDOW *w = newwin(5, 20, 8, 10);
        box(w, 0, 0);
        mvwaddstr(w, 2, 3, "in a window");
        wrefresh(w);
        refresh();
        char buf[40];
        mvinnstr(3, 4, buf, 12); buf[12] = 0;
        CHECK(!strcmp(buf, "hello curses"), "the screen holds what was written");
        delwin(w);
        endwin();
        delscreen(scr);
    }
    fflush(out);
    fseek(out, 0, SEEK_END);
    long size = ftell(out);
    rewind(out);
    char *all = malloc((size_t)size + 1);
    size_t got = fread(all, 1, (size_t)size, out); all[got] = 0;
    CHECK(size > 50 && strstr(all, "hello curses") && strstr(all, "in a window") && strstr(all, "12 x 34"), "the terminal output carries all the text");
    CHECK(strstr(all, "\033["), "and escape sequences");
    DONE("ncurses");
}
