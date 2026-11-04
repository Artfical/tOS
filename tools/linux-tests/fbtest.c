/* A Linux program's window: draws in /dev/fb0 and reads the pointer and keys from /dev/input/event0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 8;
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { printf("fbtest: no window (open /dev/fb0: errno %d)\n", errno); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &f)) { printf("fbtest: FAIL ioctl errno %d\n", errno); return 1; }
    printf("fbtest: %ux%u %u bpp, line %u, id %s\n", v.xres, v.yres, v.bits_per_pixel, f.line_length, f.id);
    uint8_t *m = mmap(0, f.line_length * v.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (m == MAP_FAILED) { printf("fbtest: FAIL mmap errno %d\n", errno); return 1; }
    for (unsigned y = 0; y < v.yres; y++) {
        uint32_t *row = (uint32_t *)(m + y * f.line_length);
        for (unsigned x = 0; x < v.xres; x++) row[x] = ((x * 255 / v.xres) << 16) | ((y * 255 / v.yres) << 8) | 0x40;
    }
    for (unsigned y = 20; y < 80; y++) for (unsigned x = 20; x < 220; x++) ((uint32_t *)(m + y * f.line_length))[x] = 0xFFFFFF;
    printf("fbtest: drew a gradient and a white box\n"); fflush(stdout);
    int ev = open("/dev/input/event0", O_RDONLY | O_NONBLOCK), kb = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (ev < 0) { printf("fbtest: FAIL open event0 errno %d\n", errno); return 1; }
    char name[64] = ""; ioctl(ev, EVIOCGNAME(sizeof(name)), name);
    printf("fbtest: input device \"%s\"\n", name); fflush(stdout);
    int clicks = 0, keys = 0, moves = 0;
    for (int t = 0; t < secs * 10; t++) {
        struct pollfd p[2] = { { ev, POLLIN, 0 }, { kb, POLLIN, 0 } };
        if (poll(p, 2, 100) > 0) {
            struct input_event e;
            for (int dv = 0; dv < 2; dv++) while (read(dv ? kb : ev, &e, sizeof(e)) == sizeof(e)) {
                if (e.type == EV_KEY && e.value == 1 && e.code >= BTN_LEFT && e.code <= BTN_MIDDLE) { clicks++; printf("fbtest: click\n"); fflush(stdout); }
                else if (e.type == EV_KEY && e.value == 1) { keys++; printf("fbtest: key %d\n", e.code); fflush(stdout); }
                else if (e.type == EV_ABS) moves++;
            }
        }
    }
    printf("fbtest: clicks=%d keys=%d pointer-moves=%d\n", clicks, keys, moves);
    printf("fbtest: all passed\n");
    return 0;
}
