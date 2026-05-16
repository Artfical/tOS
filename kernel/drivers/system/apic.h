#ifndef APIC_H
#define APIC_H
#include <stdint.h>
#define APIC_BASE 0xFEE00000
#define APIC_ID 0x20
#define APIC_VER 0x30
#define APIC_TPR 0x80
#define APIC_APR 0x90
#define APIC_PPR 0xA0
#define APIC_EOI 0xB0
#define APIC_LDR 0xD0
#define APIC_DFR 0xE0
#define APIC_SVR 0xF0
#define APIC_ISR 0x100
#define APIC_TMR 0x180
#define APIC_IRR 0x200
#define APIC_ESR 0x280
#define APIC_ICR_LOW 0x300
#define APIC_ICR_HIGH 0x310
#define APIC_LVT_TIMER 0x320
#define APIC_LVT_THERMAL 0x330
#define APIC_LVT_PERFMON 0x340
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_LINT1 0x360
#define APIC_LVT_ERROR 0x370
#define APIC_TIMER_DIV 0x3E0
#define APIC_SVR_ENABLE 0x100
#define APIC_ICR_INIT 0x500
#define APIC_ICR_STARTUP 0x600
#define APIC_ICR_DELIVERY_PENDING 0x1000
#define APIC_TIMER_MODE_ONESHOT 0x00
#define APIC_TIMER_MODE_PERIODIC 0x20000
#define APIC_TIMER_MODE_TSCDEADLINE 0x40000
typedef struct {
    int present;
    uint32_t base;
    uint8_t id;
    uint8_t version;
} apic_timer_t;
int apic_init(apic_timer_t *apic);
void apic_eoi(void);
void apic_send_ipi(uint8_t apic_id, uint8_t vec);
#endif
