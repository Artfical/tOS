#include "pcspkr.h"
#include "io.h"
void pcspkr_init(void)
{
    pcspkr_off();
}
void pcspkr_on(uint32_t freq)
{
    if (freq == 0) {
        pcspkr_off();
        return;
    }
    uint32_t div = 1193180 / freq;
    outb(PIT_CMD_PORT, 0xB6);
    outb(PIT_CH2_PORT, div & 0xFF);
    outb(PIT_CH2_PORT, (div >> 8) & 0xFF);
    uint8_t tmp = inb(PCSPKR_PORT);
    outb(PCSPKR_PORT, tmp | 0x03);
}
void pcspkr_off(void)
{
    uint8_t tmp = inb(PCSPKR_PORT);
    outb(PCSPKR_PORT, tmp & 0xFC);
}
void pcspkr_beep(uint32_t freq, uint32_t duration_ms)
{
    pcspkr_on(freq);
    for (volatile uint32_t i = 0; i < duration_ms * 10000; i++);
    pcspkr_off();
}
