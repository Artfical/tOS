#include "tsc.h"
static uint64_t tsc_read_raw(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
int tsc_init(tsc_timer_t *tsc)
{
    tsc->present = 0;
    tsc->frequency = 0;
    tsc->present = 1;
    return 0;
}
uint64_t tsc_read(void)
{
    return tsc_read_raw();
}
void tsc_delay_us(tsc_timer_t *tsc, uint64_t us)
{
    if (!tsc->frequency) return;
    uint64_t target = tsc_read() + tsc->frequency * us / 1000000;
    while (tsc_read() < target);
}
