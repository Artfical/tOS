#ifndef IOAPIC_H
#define IOAPIC_H
#include <stdint.h>
#define IOAPIC_BASE 0xFEC00000
#define IOAPIC_ID 0x00
#define IOAPIC_VER 0x01
#define IOAPIC_ARB 0x02
#define IOAPIC_REDIR_TBL 0x10
typedef struct {
    int present;
    uint32_t base;
    int irq_count;
} ioapic_device_t;
int ioapic_init(ioapic_device_t *ioapic);
void ioapic_set_irq(ioapic_device_t *ioapic, int irq, uint8_t vector, int enabled);
#endif
