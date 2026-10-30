#include "isr.h"
#include "linux_abi.h"
#include "paging.h"
#include "terminal.h"
#include "io.h"
#include "debugmon.h"
#include "scheduler.h"
#include "proc.h"
#include "serial.h"
#include "string.h"
#include "syscall.h"

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


/* a 64-bit value: its low 32 bits, or all 64 when the upper half is used (kernel addresses) */
static void hex16(char *buf, uint64_t val)
{
    int n = (val >> 32) ? 16 : 8;
    for (int i = n - 1; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[val & 0xF];
        val >>= 4;
    }
    buf[n] = '\0';
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
                           uint64_t cr2, uint64_t eip, uint64_t err_code, int fake,
                           const char *extra_line)
{
    char buf[17];

    crash_active = 1;
    if (!fake) scheduler_debug_dump();
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
    hex16(buf, cr2);
    terminal_writestring(buf);
    terminal_writestring("   RIP: ");
    hex16(buf, eip);
    terminal_writestring(buf);
    terminal_writestring("   ERR: ");
    hex16(buf, err_code);
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
    /* a 64-bit program's page that is not there yet (from the program, or the kernel on its
     * behalf): it comes now if the program may have it */
    if (regs->int_no == 14) {
        uint64_t addr;
        asm volatile("mov %%cr2, %0" : "=r"(addr));
        /* a write to a page shared since a fork */
        if (addr < PAGING_USER_END && (regs->err_code & 3u) == 3u &&
            paging_cow_break(paging_current_dir(), addr) == 1) return;
        if (addr < PAGING_USER_END && linux64_page_fault(addr, regs->err_code)) return;
    }
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
        return;
    }

    if (regs->int_no < 32 && (regs->cs & 3) == 3 && proc_current()) {
        /* a user program misbehaved: that program ends, the system carries on */
        proc_t *p = proc_current();
        uint64_t cr2 = 0;
        if (regs->int_no == 14) asm volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_writestring("\n");
        terminal_writestring(p->name);
        terminal_writestring(": ");
        terminal_writestring(regs->int_no == 14 ? "segmentation fault" : exception_messages[regs->int_no]);
        terminal_writestring(" (killed)\n");
        {
            /* where it happened, for the serial log: eip, the faulting address, the error code */
            char line[96] = "user fault: eip=", h[12];
            uint32_t vals[4] = { regs->eip, cr2, regs->err_code, regs->useresp };
            static const char *const names[4] = { "", " cr2=", " err=", " esp=" };
            for (int k = 0; k < 4; k++) {
                if (k) strcat(line, names[k]);
                hex8(h, vals[k]);
                strcat(line, h);
            }
            strcat(line, "\n");
            serial_write(line);
            {
                char l2[200] = "user fault 64: rip=", h2[20];
                uint64_t v2[8] = { regs->rip, cr2, regs->rax, regs->rdi, regs->rsi, regs->rdx, regs->rcx, regs->rsp };
                static const char *const n2[8] = { "", " cr2=", " rax=", " rdi=", " rsi=", " rdx=", " rcx=", " rsp=" };
                for (int k = 0; k < 8; k++) { if (k) strcat(l2, n2[k]); hex16(h2, v2[k]); strcat(l2, h2); }
                strcat(l2, "\n");
                serial_write(l2);
            }
            /* the stack, to find the calls that led here */
            uint32_t sp = regs->useresp;
            if (sp >= 0x80000000u && sp < 0xC0000000u && !(sp & 3)) {
                uint32_t words = (0xC0000000u - sp) / 4;
                if (words > 96) words = 96;
                for (uint32_t w = 0; w < words; w += 8) {
                    char row[96] = "  stack ";
                    for (uint32_t k = w; k < w + 8 && k < words; k++) {
                        hex8(h, ((const uint32_t *)(uintptr_t)sp)[k]);
                        strcat(row, h); strcat(row, " ");
                    }
                    strcat(row, "\n");
                    serial_write(row);
                }
            }
            syscall_dump_recent();
        }
        {
            /* what wait() says ended it */
            proc_t *fp = proc_current();
            if (fp) fp->term_sig = regs->int_no == 0 ? 8 : regs->int_no == 6 ? 4 : regs->int_no == 3 ? 5 : 11;
        }
        if (task_current() && task_current()->user_thread)
            proc_thread_exit(-(int)regs->int_no - 128);
        proc_exit(-(int)regs->int_no - 128);
    }

    if (regs->int_no < 32) {
        debugmon_log_line(exception_messages[regs->int_no]);

        uint64_t cr2 = 0;
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
        const char *lbl1 = "RSP:";
        while (*lbl1) extra[p++] = *lbl1++;
        for (int nib = 60; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->rsp >> nib) & 0xF];
        const char *lbl2 = " RBP:";
        while (*lbl2) extra[p++] = *lbl2++;
        for (int nib = 60; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->rbp >> nib) & 0xF];
        const char *lbl3 = " CS:";
        while (*lbl3) extra[p++] = *lbl3++;
        for (int nib = 4; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(regs->cs >> nib) & 0xF];
        const char *lbl4 = " PID:";
        while (*lbl4) extra[p++] = *lbl4++;
        task_t *cur = task_current();
        uint32_t pid = cur ? cur->pid : 0xFFFFFFFF;
        for (int nib = 28; nib >= 0; nib -= 4) extra[p++] = "0123456789ABCDEF"[(pid >> nib) & 0xF];
        extra[p] = 0;

        crash_screen_trigger(exception_messages[regs->int_no], (int)regs->int_no,
                              cr2, regs->rip, regs->err_code, 0, extra);
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
