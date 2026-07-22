#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* http_get() propagates whichever internal call failed -- tcp_connect()'s
 * TCP_ERR_* (or an ARP/IP code tcp_connect() itself propagated) --
 * verbatim (see tcp.h), rather than collapsing every failure into a
 * single generic value a caller can't tell apart.
 * `ip` must already be resolved by the caller -- `host` is only used
 * for the request's Host: header, never re-resolved here (a second,
 * independent DNS lookup right after the caller's own successful one
 * could itself fail and abort before tcp_connect() was ever reached,
 * misreporting a DNS hiccup as a connection error). */
int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len);
const char *http_strerror(int err);

#endif
