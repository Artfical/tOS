#ifndef FBWIN_GUARD_H
#define FBWIN_GUARD_H

#include <stdint.h>

/* A window a Linux program draws in: /dev/fb0 is its pixels (shared pages the program maps) and
 * /dev/input/event0 is what the person does in it (pointer, buttons, keys, as evdev events). */

#define TFBW_W 800
#define TFBW_H 560
#define TFBW_STRIDE 4096          /* bytes of a row: one page */

int      fbwin_open(void);                          /* the window exists (made now when needed): 0, or -1 without a desktop */
int      fbwin_active(void);
uint32_t fbwin_page(int row);                       /* the physical page holding a row of pixels, or 0 */
int      fbwin_ioctl_fb(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t));      /* 0 ok, negative errno, 1 not mine */
int      fbwin_ioctl_ev(int dev, uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t));
int      fbwin_ioctl_vt(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t));      /* /dev/tty0: the virtual-terminal calls an X server makes: all accepted */
int      fbwin_ev_read(int dev, uint8_t *out, int max_bytes);   /* whole 24-byte events: bytes, 0 when none */
int      fbwin_ev_ready(int dev);
int      fbwin_ioctl_drm(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t));     /* /dev/dri/card0 (dumb buffers): 0 ok, negative errno, 1 not mine */
uint32_t fbwin_drm_page(uint64_t off);              /* the page of a dumb buffer at an mmap offset, or 0 */
int      fbwin_drm_read(uint8_t *out, int max_bytes);   /* whole 32-byte flip-complete events */
int      fbwin_drm_ready(void);
void     fbwin_owner_gone(uint32_t pid);            /* a program ended: its window closes */
void     fbwin_set_owner(uint32_t pid);

#endif
