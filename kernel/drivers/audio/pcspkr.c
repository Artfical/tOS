#include "pcspkr.h"
#include "io.h"
#include "debugmon.h"
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
    /* Real wall-clock wait (TSC-backed, see debugmon.c) instead of a
     * fixed spin count — an arbitrary iteration count has no defined
     * relationship to real time and made every beep's actual duration
     * depend on host/hypervisor CPU speed. */
    uint32_t deadline = debugmon_uptime_ms() + duration_ms;
    while (debugmon_uptime_ms() < deadline) { }
    pcspkr_off();
}
