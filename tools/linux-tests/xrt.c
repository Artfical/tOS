/* How long does one round trip to the X server take? 300 XInternAtom calls (each waits for the answer), timed.
 *   gcc xrt.c -o xrt -lX11     then   tos-x xrt */
#include <X11/Xlib.h>
#include <stdio.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

int main(void)
{
    double t0 = now();
    Display *d = XOpenDisplay(0);
    if (!d) { printf("xrt: cannot open the display\n"); return 1; }
    double t1 = now();
    char name[32];
    for (int i = 0; i < 300; i++) { snprintf(name, sizeof name, "XRT_ATOM_%d", i); XInternAtom(d, name, False); }
    double t2 = now();
    for (int i = 0; i < 100; i++) XSync(d, False);
    double t3 = now();
    printf("xrt: open %.0f ms; 300 InternAtom %.0f ms (%.1f ms each); 100 XSync %.0f ms (%.1f ms each)\n", t1 - t0, t2 - t1, (t2 - t1) / 300, t3 - t2, (t3 - t2) / 100);
    XCloseDisplay(d);
    return 0;
}
