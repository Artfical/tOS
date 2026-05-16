#ifndef PCSPKR_H
#define PCSPKR_H
#include <stdint.h>
#define PCSPKR_PORT 0x61
#define PIT_CH2_PORT 0x42
#define PIT_CMD_PORT 0x43
void pcspkr_init(void);
void pcspkr_beep(uint32_t freq, uint32_t duration_ms);
void pcspkr_on(uint32_t freq);
void pcspkr_off(void);
#endif
