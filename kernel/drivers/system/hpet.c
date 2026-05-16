#include "hpet.h"
#include "io.h"
#include "acpi.h"
int hpet_init(hpet_timer_t *hpet)
{
    hpet->base = HPET_BASE;
    volatile uint64_t *cap = (volatile uint64_t *)hpet->base;
    uint64_t cap_val = *cap;
    if (!(cap_val & 0xFFFFFFFF)) return -1;
    uint32_t period = (cap_val >> 32) & 0xFFFFFFFF;
    if (period == 0) return -1;
    hpet->frequency = 1000000000000000ULL / period;
    hpet->present = 1;
    volatile uint64_t *cfg = (volatile uint64_t *)(hpet->base + HPET_GEN_CFG);
    *cfg |= 1;
    return 0;
}
uint64_t hpet_read_counter(hpet_timer_t *hpet)
{
    volatile uint64_t *cnt = (volatile uint64_t *)(hpet->base + HPET_MAIN_CNT);
    return *cnt;
}
void hpet_sleep(hpet_timer_t *hpet, uint64_t ms)
{
    uint64_t target = hpet_read_counter(hpet) + (hpet->frequency / 1000) * ms;
    while (hpet_read_counter(hpet) < target);
}
