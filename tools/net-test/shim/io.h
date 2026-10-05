#include <stdint.h>
static inline void outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
static inline uint8_t inb(uint16_t p) { (void)p; static uint8_t g; return g++; }
