#include "apic.h"
#include "io.h"
#include "msr.h"
int apic_init(apic_timer_t *apic)
{
    apic->present = 0;
    uint32_t eax, edx;
    msr_read(0x1B, &eax, &edx);
    if (!(edx & (1 << 9))) return -1;
    apic->base = APIC_BASE;
    msr_write(0x1B, (apic->base & 0xFFFFFFF0) | 0x800, 0);
    volatile uint32_t *regs = (volatile uint32_t *)apic->base;
    regs[APIC_SVR / 4] = APIC_SVR_ENABLE | 0xFF;
    apic->id = regs[APIC_ID / 4] >> 24;
    apic->version = regs[APIC_VER / 4] & 0xFF;
    regs[APIC_TPR / 4] = 0;
    apic->present = 1;
    return 0;
}
void apic_eoi(void)
{
    volatile uint32_t *regs = (volatile uint32_t *)APIC_BASE;
    regs[APIC_EOI / 4] = 0;
}
void apic_send_ipi(uint8_t apic_id, uint8_t vec)
{
    volatile uint32_t *regs = (volatile uint32_t *)APIC_BASE;
    regs[APIC_ICR_HIGH / 4] = apic_id << 24;
    regs[APIC_ICR_LOW / 4] = vec | 0x4000;
    while (regs[APIC_ICR_LOW / 4] & APIC_ICR_DELIVERY_PENDING);
}
