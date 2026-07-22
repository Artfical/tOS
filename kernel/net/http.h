#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* http_get() propagates whichever internal call failed -- dns_resolve()'s
 * DNS_ERR_* or tcp_connect()'s TCP_ERR_* (or an ARP/IP code tcp_connect()
 * itself propagated) -- verbatim (see dns.h/tcp.h), rather than collapsing
 * every failure into a single generic value a caller can't tell apart. */
int http_get(const char *host, uint16_t port, const char *path, uint8_t *response, int max_len);
const char *http_strerror(int err);

#endif
