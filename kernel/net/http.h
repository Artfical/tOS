#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

int http_get(const char *host, uint16_t port, const char *path, uint8_t *response, int max_len);

#endif
