#include "isr.h"
#include "terminal.h"
#include "io.h"
#include "debugmon.h"
#include "scheduler.h"

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

static int crash_active = 0;

int crash_screen_is_active(void)
{
    return crash_active;
}

void crash_screen_clear(void)
{
    crash_active = 0;
}

void crash_screen_trigger(const char *exception_name, int exception_code,
                           uint32_t cr2, uint32_t eip, uint32_t err_code, int fake,
                           const char *extra_line)
{
    char buf[9];

    crash_active = 1;
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

    if (extra_line) {
        terminal_writestring("        ");
        terminal_writestring(extra_line);
        terminal_writestring("\n\n");
    }

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

        /* Extra forensic line for a crash that's proven hard to
         * reproduce off real hardware (see task_current()/regs->cs
         * below) -- ESP/EBP at fault time, CS (which privilege level
         * the fault happened in), and the currently running task's
         * pid/name, so a single crash-screen photo carries much more
         * of what a debugger would normally show. */
        char extra[96];
        int p = 0;
        const char *lbl1 = "ESP:";
        while (*lbl1) extra[p++] = *lbl1++;
        for (int nib = 28; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->esp >> nib) & 0xF];
        const char *lbl2 = " EBP:";
        while (*lbl2) extra[p++] = *lbl2++;
        for (int nib = 28; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->ebp >> nib) & 0xF];
        const char *lbl3 = " CS:";
        while (*lbl3) extra[p++] = *lbl3++;
        for (int nib = 12; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->cs >> nib) & 0xF];
        const char *lbl4 = " PID:";
        while (*lbl4) extra[p++] = *lbl4++;
        task_t *cur = task_current();
        uint32_t pid = cur ? cur->pid : 0xFFFFFFFF;
        for (int nib = 28; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(pid >> nib) & 0xF];
        extra[p] = 0;

        crash_screen_trigger(exception_messages[regs->int_no], (int)regs->int_no,
                              cr2, regs->eip, regs->err_code, 0, extra);
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
