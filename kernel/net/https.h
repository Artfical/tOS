#ifndef HTTPS_H
#define HTTPS_H

#include <stdint.h>

/* HTTPS GET over TLS 1.2.  Returns total bytes received (header + body),
 * or -1 on error. No certificate verification (hobby OS).
 * `ip` must already be resolved by the caller -- `host` is only used
 * for the request's Host: header, never re-resolved here (a second,
 * independent DNS lookup right after the caller's own successful one
 * could itself fail and abort before tls_connect() was ever reached,
 * misreporting a DNS hiccup as a TLS/connection error). */
int https_get(uint32_t ip, const char *host, uint16_t port, const char *path,
              uint8_t *response, int max_len);

#endif
