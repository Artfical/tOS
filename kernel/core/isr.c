#include "isr.h"
#include "terminal.h"
#include "io.h"
#include "debugmon.h"

isr_handler_t interrupt_handlers[256];
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
        debugmon_log_line(exception_messages[regs->int_no]);

        terminal_setcolor(0x4F);
        terminal_writestring("\nKERNEL PANIC: ");
        terminal_writestring(exception_messages[regs->int_no]);
        terminal_writestring(" (Exception ");
        terminal_write("0123456789ABCDEF" + (regs->int_no >> 4), 1);
        terminal_write("0123456789ABCDEF" + (regs->int_no & 0xF), 1);
        terminal_writestring(")\n");

        if (regs->int_no == 14) {
            uint32_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            terminal_writestring("CR2 (fault address): ");
            char buf[9];
            for (int i = 7; i >= 0; i--) {
                buf[i] = "0123456789ABCDEF"[cr2 & 0xF];
                cr2 >>= 4;
            }
            buf[8] = '\0';
            terminal_writestring(buf);
            terminal_writestring("\n");
        }

        char buf[9];
        void print_field(const char *label, uint32_t val) {
            terminal_writestring(label);
            for (int i = 7; i >= 0; i--) {
                buf[i] = "0123456789ABCDEF"[val & 0xF];
                val >>= 4;
            }
            buf[8] = '\0';
            terminal_writestring(buf);
            terminal_writestring(" ");
        }
        print_field("EIP=", regs->eip);
        print_field("CS=", regs->cs);
        print_field("EFL=", regs->eflags);
        print_field("USP=", regs->useresp);
        print_field("SS=", regs->ss);
        print_field("ERR=", regs->err_code);
        terminal_writestring("\n");
        print_field("EAX=", regs->eax);
        print_field("EBX=", regs->ebx);
        print_field("ECX=", regs->ecx);
        print_field("EDX=", regs->edx);
        print_field("ESP=", regs->esp);
        print_field("EBP=", regs->ebp);
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
