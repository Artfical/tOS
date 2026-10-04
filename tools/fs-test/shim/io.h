#include <stdint.h>
#include <time.h>
/* CMOS shim: serve the host clock in BCD through ports 0x70/0x71 */
static uint8_t shim_reg;
static inline void outb(uint16_t port, uint8_t v) { if (port == 0x70) shim_reg = v; }
static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline uint8_t inb(uint16_t port)
{
    (void)port;
    time_t t = time(0);
    struct tm *g = gmtime(&t);
    switch (shim_reg) {
    case 0x00: return bcd(g->tm_sec);
    case 0x02: return bcd(g->tm_min);
    case 0x04: return bcd(g->tm_hour);
    case 0x07: return bcd(g->tm_mday);
    case 0x08: return bcd(g->tm_mon + 1);
    case 0x09: return bcd(g->tm_year % 100);
    case 0x32: return bcd((g->tm_year + 1900) / 100);
    case 0x0A: return 0;
    case 0x0B: return 0x02;   /* BCD, 24h */
    }
    return 0;
}
