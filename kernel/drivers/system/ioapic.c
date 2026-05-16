#include "ioapic.h"
#include "io.h"
static uint32_t ioapic_read(ioapic_device_t *ioapic, uint8_t reg)
{
    volatile uint32_t *base = (volatile uint32_t *)ioapic->base;
    base[0] = reg;
    return base[4];
}
static void ioapic_write(ioapic_device_t *ioapic, uint8_t reg, uint32_t val)
{
    volatile uint32_t *base = (volatile uint32_t *)ioapic->base;
    base[0] = reg;
    base[4] = val;
}
int ioapic_init(ioapic_device_t *ioapic)
{
    ioapic->base = IOAPIC_BASE;
    uint32_t ver = ioapic_read(ioapic, IOAPIC_VER);
    ioapic->irq_count = (ver >> 16) & 0xFF;
    ioapic->present = 1;
    return 0;
}
void ioapic_set_irq(ioapic_device_t *ioapic, int irq, uint8_t vector, int enabled)
{
    int reg = IOAPIC_REDIR_TBL + irq * 2;
    uint32_t low = vector | (enabled ? 0 : 1 << 16);
    uint32_t high = 0;
    ioapic_write(ioapic, reg, low);
    ioapic_write(ioapic, reg + 1, high);
}
