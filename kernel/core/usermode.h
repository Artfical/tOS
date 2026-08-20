#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

/* Fixed addresses for the flat, non-relocatable `.t` binary model
 * (see cmd_run() in cmd_net.c and Desktop/tos_sdk/user.ld) -- every
 * `.t` file is compiled once, ahead of time, linked against a single
 * absolute load address baked into its code (string/data references
 * use absolute 32-bit immediates, not PC-relative addressing), so
 * the loader MUST place it at exactly that same address on every
 * machine. That ruled out computing this from actual detected RAM at
 * boot (tried that -- it just moved the mismatch to be between the
 * loader and every already-compiled .t binary instead of fixing it).
 *
 * Must stay above whatever total physical RAM the target machines
 * have: paging_init() identity-maps [0, total_mem) without
 * PTE_USER, so any address inside that range is present-but-
 * inaccessible from ring3 (a protection-violation page fault, not a
 * clean crash). 0x50000000 (1.25GB) was the original value and
 * worked on the 1024MB QEMU config this shipped with, but faulted on
 * real hardware with 1552MB RAM. Raised to 0x80000000 (2GB, ~500MB
 * of margin above 1552MB) with USER_STACK_TOP left as it already was
 * (0xBFFFF000, ~3.2GB -- comfortably clear either way), leaving over
 * 1GB of gap between code and stack. */
#define USER_CODE_BASE 0x80000000
#define USER_STACK_TOP 0xBFFFF000
#define USER_CODE_MAX_SIZE 0x2000000 /* 32MB ceiling for a single .t binary, matches cmd_run()'s own limit */
#define USER_STACK_PAGES   16        /* 64KB, matches usermode_init()'s own mapping */

void enter_user_mode(uint32_t entry, uint32_t user_stack_top);
void usermode_init(void);
void sys_exit_set_jmp(uint32_t esp, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t eip);
void __attribute__((noreturn)) sys_exit_longjmp(void);

#endif
