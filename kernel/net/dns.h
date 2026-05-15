#ifndef DNS_H
#define DNS_H

#include <stdint.h>

int dns_resolve(const char *hostname, uint32_t *ip_out);

#endif
