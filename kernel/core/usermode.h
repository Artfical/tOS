#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

void enter_user_mode(uint32_t entry, uint32_t user_stack_top);
void usermode_init(void);
void sys_exit_set_jmp(uint32_t esp, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t eip);
void __attribute__((noreturn)) sys_exit_longjmp(void);

#endif
