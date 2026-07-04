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

extern isr_handler_t interrupt_handlers[256];

extern uint32_t isr_stub_table[];

#endif
