/* An X client: a window, drawing, a core font, and the events the X server gets from the pointer and the keys */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("xtest: FAIL cannot open the display\n"); return 1; }
    int s = DefaultScreen(d);
    printf("xtest: connected to %s, screen %dx%d, depth %d\n", DisplayString(d), DisplayWidth(d, s), DisplayHeight(d, s), DefaultDepth(d, s));
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), 40, 40, 400, 300, 2, BlackPixel(d, s), WhitePixel(d, s));
    XStoreName(d, w, "xtest");
    XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask | PointerMotionMask);
    XMapWindow(d, w);
    GC gc = XCreateGC(d, w, 0, NULL);
    XFontStruct *f = XLoadQueryFont(d, "fixed");
    printf("xtest: font fixed %s\n", f ? "loaded" : "MISSING");
    if (f) XSetFont(d, gc, f->fid);
    int clicks = 0, keys = 0, expose = 0, moves = 0;
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        while (XPending(d)) {
            XEvent e; XNextEvent(d, &e);
            if (e.type == Expose && e.xexpose.count == 0) {
                expose++;
                XSetForeground(d, gc, 0x2040C0); XFillRectangle(d, w, gc, 20, 20, 360, 120);
                XSetForeground(d, gc, 0xFFFFFF); XDrawString(d, w, gc, 40, 70, "Hello from an X client on tOS", 29);
                XSetForeground(d, gc, 0xC02020); XFillArc(d, w, gc, 150, 160, 100, 100, 0, 360 * 64);
                XFlush(d);
            } else if (e.type == ButtonPress) { clicks++; printf("xtest: button %u at %d,%d\n", e.xbutton.button, e.xbutton.x, e.xbutton.y); fflush(stdout); }
            else if (e.type == KeyPress) { keys++; char buf[16]; KeySym ks; int n = XLookupString(&e.xkey, buf, 15, &ks, NULL); buf[n > 0 ? n : 0] = 0; printf("xtest: key %s (%s)\n", XKeysymToString(ks), buf); fflush(stdout); }
            else if (e.type == MotionNotify) moves++;
        }
        usleep(20000);
    }
    printf("xtest: expose=%d clicks=%d keys=%d moves=%d\n", expose, clicks, keys, moves);
    printf(expose ? "xtest: all passed\n" : "xtest: FAIL nothing was drawn\n");
    XCloseDisplay(d);
    return expose == 0;
}
