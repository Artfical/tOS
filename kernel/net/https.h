#ifndef HTTPS_H
#define HTTPS_H

#include <stdint.h>

/* HTTPS GET over TLS 1.2.  Returns total bytes received (header + body),
 * or -1 on error. No certificate verification (hobby OS). */
int https_get(const char *host, const char *path,
              uint8_t *response, int max_len);

#endif
