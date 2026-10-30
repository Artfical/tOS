#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* The terminal of a Linux program (see tty.c). */
int  tty_read(uint8_t *buf, uint32_t count, int nonblock);   /* bytes, 0 = end of file, or -errno */
int  tty_ready(void);                                        /* a read would not wait */
void tty_note_output(void);                                  /* a Linux program writes: it gets UTF-8 and key sequences */
/* terminal ioctls: 0 = done, -errno, or 1 = not one of ours; the callers check user memory */
int  tty_ioctl(uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t), int (*rd_ok)(uint64_t, uint64_t));
void tty_detach(void *surface);                              /* the program is gone: the shell's terminal again */

#endif
