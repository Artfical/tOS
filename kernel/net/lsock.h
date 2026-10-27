#ifndef LSOCK_H
#define LSOCK_H

#include <stdint.h>

/* Linux sockets for ELF programs (Node, npm): IPv4 TCP and UDP over tOS's own network stack.
 * Every call answers at once; blocking ones are done by the caller (syscall.c) waiting with
 * lsock_pump() in between. Results are Linux error numbers, negative. */

#define LSOCK_MAX 32

int      lsock_create(int domain, int type, int protocol);
void     lsock_close(int s);
int      lsock_bind(int s, const uint8_t *addr, uint32_t len);
int      lsock_connect_start(int s, const uint8_t *addr, uint32_t len);
int      lsock_connect_check(int s);          /* 0 connected, -EINPROGRESS still going, or an error */
int      lsock_send(int s, const uint8_t *data, uint32_t len, const uint8_t *to, uint32_t tolen);
int      lsock_recv(int s, uint8_t *buf, uint32_t len, uint8_t *from, uint32_t *fromlen, int peek);
uint32_t lsock_ready(int s);                  /* EPOLLIN/OUT/ERR/HUP/RDHUP bits */
int      lsock_getsockopt(int s, int level, int name, int *value);
int      lsock_name(int s, int peer, uint8_t *addr, uint32_t *len);
int      lsock_shutdown(int s, int how);
int      lsock_pending(int s);                /* bytes that can be read now (FIONREAD) */
int      lsock_is_stream(int s);
void     lsock_pump(void);                    /* moves the network along, if any socket is open */

#endif
