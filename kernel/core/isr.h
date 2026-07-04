#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags, useresp, ss;
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
                           uint32_t cr2, uint32_t eip, uint32_t err_code, int fake);

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

extern uint32_t isr_stub_table[];

#endif
