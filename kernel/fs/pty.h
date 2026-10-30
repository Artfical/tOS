#ifndef PTY_H
#define PTY_H

#include <stdint.h>

/* Pseudo-terminals for Linux programs (a login server, a terminal emulator, script): /dev/ptmx opens the
 * master of a new pair, /dev/pts/N the slave. What the master writes goes through the line discipline to
 * the slave's program (cooked lines with editing and echo, ^C, or raw bytes); what the slave's program
 * writes comes out of the master. */

#define PTY_MAX 8

int  pty_new(void);                                        /* the master of a new pair: its number, or -errno */
int  pty_slave_open(int n);                                /* 0, or -errno (no such pair, still locked) */
int  pty_exists(int n);
void pty_ref(int n, int slave);                            /* a descriptor was copied */
void pty_close(int n, int slave);                          /* a descriptor was closed */
int  pty_read(int n, int slave, uint8_t *buf, uint32_t count, int nonblock);    /* bytes, 0 = end, or -errno */
int  pty_write(int n, int slave, const uint8_t *buf, uint32_t count, int nonblock);
uint32_t pty_ready(int n, int slave);                      /* EPOLLIN/OUT/HUP bits */
/* ioctls: 0 = done, -errno, or 1 = not one of ours */
int  pty_ioctl(int n, int slave, uint32_t req, uint64_t arg, int (*wr_ok)(uint64_t, uint64_t), int (*rd_ok)(uint64_t, uint64_t));

#endif
