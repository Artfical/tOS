#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *);

void isr_init(void);
void isr_register_handler(uint8_t num, isr_handler_t handler);
void isr_handler(registers_t *regs);

extern uint32_t isr_stub_table[];

#endif
