#ifndef IRQ_H
#define IRQ_H

#include "isr.h"

void irq_init(void);
void irq_handler(registers_t *regs);
void irq_ack(int irq);
uint32_t irq_get_tick(void);

#endif
