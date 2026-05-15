#include "isr.h"
#include "terminal.h"
#include "io.h"

static isr_handler_t interrupt_handlers[256];
static const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

__attribute__((naked)) void isr_common_stub(void)
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
        "call isr_handler\n"
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

void isr_handler(registers_t *regs)
{
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
        return;
    }

    if (regs->int_no < 32) {
        terminal_setcolor(0x4F);
        terminal_writestring("\nKERNEL PANIC: ");
        terminal_writestring(exception_messages[regs->int_no]);
        terminal_writestring(" (Exception ");
        terminal_write("0123456789ABCDEF" + (regs->int_no >> 4), 1);
        terminal_write("0123456789ABCDEF" + (regs->int_no & 0xF), 1);
        terminal_writestring(")\n");
        terminal_writestring("EIP: ");
        char buf[9];
        for (int i = 7; i >= 0; i--) {
            buf[i] = "0123456789ABCDEF"[regs->eip & 0xF];
            regs->eip >>= 4;
        }
        buf[8] = '\0';
        terminal_writestring(buf);
        terminal_writestring("\nSystem halted.\n");
        for (;;) { asm volatile("hlt"); }
    }
}

void isr_register_handler(uint8_t num, isr_handler_t handler)
{
    interrupt_handlers[num] = handler;
}

void isr_init(void)
{
    for (int i = 0; i < 256; i++)
        interrupt_handlers[i] = NULL;
}
