#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* What an interrupt, exception or int 0x80 leaves on the kernel stack (isr_stubs.s pushes all but
 * the last five, which the CPU pushes). A 32-bit program's registers are the low halves: the old
 * names (eax, eip, useresp...) are those halves, so code written for them keeps working, while
 * 64-bit code uses rax, rip, rsp... Writing an e-name only writes the low 32 bits. */
#define REG64(n32, n64) union { uint64_t n64; uint32_t n32; }
typedef struct {
    uint64_t gs, fs, es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    REG64(edi, rdi);
    REG64(esi, rsi);
    REG64(ebp, rbp);
    REG64(esp, rsp_unused);
    REG64(ebx, rbx);
    REG64(edx, rdx);
    REG64(ecx, rcx);
    REG64(eax, rax);
    uint64_t int_no;
    uint64_t err_code;
    REG64(eip, rip);
    uint64_t cs;
    REG64(eflags, rflags);
    REG64(useresp, rsp);
    uint64_t ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *);

void isr_init(void);
void isr_register_handler(uint8_t num, isr_handler_t handler);
void isr_handler(registers_t *regs);

/* Draws the full-screen red panic display and, unless `fake`, hangs
 * the machine forever (a real halt). With `fake` set it returns
 * immediately after drawing instead -- used by the `crash` shell
 * command to show the exact same screen for a demo without actually
 * taking the system down. */
void crash_screen_trigger(const char *exception_name, int exception_code,
                           uint64_t cr2, uint64_t eip, uint64_t err_code, int fake,
                           const char *extra_line);

/* True while a (real or fake) crash screen is on-screen. The desktop's
 * own task keeps running and repainting even while another task is
 * blocked showing this screen (preemptive scheduling), so anything
 * that flushes to real VGA memory must check this first or it will
 * overwrite the crash screen on the very next timer tick. */
int crash_screen_is_active(void);

/* Called by the `crash` command once the fake screen has been
 * dismissed, letting the desktop repaint again. */
void crash_screen_clear(void);

extern isr_handler_t interrupt_handlers[256];

extern uint64_t isr_stub_table[];

#endif
