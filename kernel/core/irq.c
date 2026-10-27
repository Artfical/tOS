#include "irq.h"
#include "isr.h"
#include "io.h"
#include "terminal.h"
#include "debugmon.h"

extern void irq_common_stub(void);

void irq_ack(int irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}


void irq_handler(registers_t *regs)
{
    if (regs->int_no == 32)
        debugmon_tick();

    if (interrupt_handlers[regs->int_no])
        interrupt_handlers[regs->int_no](regs);

    if (regs->int_no >= 40)
        outb(0xA0, 0x20);

    outb(0x20, 0x20);
}

void irq_init(void)
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}
