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

static void hex8(char *buf, uint32_t val)
{
    for (int i = 7; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

void crash_screen_trigger(const char *exception_name, int exception_code,
                           uint32_t cr2, uint32_t eip, uint32_t err_code, int fake)
{
    char buf[9];

    terminal_set_force_direct(1);
    terminal_fill_screen(0x4F);
    terminal_setcolor(0x4F);
    terminal_setpos(0, 0);

    terminal_writestring("\n\n\n");
    terminal_writestring("                                     :(\n\n");
    terminal_writestring("        Your system ran into a problem and needs to restart.\n\n");
    terminal_writestring("        KERNEL PANIC: ");
    terminal_writestring(exception_name);
    terminal_writestring(" (Exception ");
    terminal_write("0123456789ABCDEF" + ((exception_code >> 4) & 0xF), 1);
    terminal_write("0123456789ABCDEF" + (exception_code & 0xF), 1);
    terminal_writestring(")\n\n");

    terminal_writestring("        CR2: ");
    hex8(buf, cr2);
    terminal_writestring(buf);
    terminal_writestring("   EIP: ");
    hex8(buf, eip);
    terminal_writestring(buf);
    terminal_writestring("   ERR: ");
    hex8(buf, err_code);
    terminal_writestring(buf);
    terminal_writestring("\n\n");

    terminal_writestring("        Your system is halted.\n");
    terminal_writestring("        Please contact us via mail in help@artfical.com\n");

    if (!fake) {
        for (;;) { asm volatile("hlt"); }
    }
}

void isr_handler(registers_t *regs)
{
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
        return;
    }

    if (regs->int_no < 32) {
        debugmon_log_line(exception_messages[regs->int_no]);

        uint32_t cr2 = 0;
        if (regs->int_no == 14) {
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
        }

        crash_screen_trigger(exception_messages[regs->int_no], (int)regs->int_no,
                              cr2, regs->eip, regs->err_code, 0);
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
