#include "irq.h"
#include "isr.h"
#include "io.h"
#include "terminal.h"

extern void irq_common_stub(void);

volatile uint32_t system_tick = 0;

static void pit_tick(registers_t *regs)
{
    (void)regs;
    system_tick++;
}

uint32_t irq_get_tick(void)
{
    return system_tick;
}

void irq_ack(int irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

__attribute__((naked)) void irq_common_stub(void)
{
    asm volatile(
        "pusha\n"
        "push %ds\n"
        "push %es\n"
        "push %fs\n"
        "push %gs\n"
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n"
        "mov %ax, %es\n"
        "mov %ax, %fs\n"
        "mov %ax, %gs\n"
        "mov %esp, %eax\n"
        "push %eax\n"
        "call irq_handler\n"
        "pop %eax\n"
        "pop %gs\n"
        "pop %fs\n"
        "pop %es\n"
        "pop %ds\n"
        "popa\n"
        "add $8, %esp\n"
        "iret\n"
    );
}

void irq_handler(registers_t *regs)
{
    if (interrupt_handlers[regs->int_no])
        interrupt_handlers[regs->int_no](regs);

    if (regs->int_no >= 40)
        outb(0xA0, 0x20);

    outb(0x20, 0x20);
}

void irq_init(void)
{
    isr_register_handler(32, pit_tick);
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
