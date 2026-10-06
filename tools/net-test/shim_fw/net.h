#include <stdint.h>
#define htons(x) ((uint16_t)((((x) >> 8) & 0xFF) | (((x) & 0xFF) << 8)))
#define ntohs(x) htons(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)
#define IP4(a,b,c,d) ((uint32_t)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))
