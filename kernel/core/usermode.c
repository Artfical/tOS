#include "usermode.h"
#include "tss.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"
#include "gdt.h"

/* USER_STACK_PAGES (64 KB — enough headroom for nested libc calls)
 * comes from usermode.h now, shared with syscall.c's pointer-range
 * validation. */

void usermode_init(void)
{
    /* every process gets its own stack when it is started (see proc_spawn()) */
    terminal_writestring("[OK] User mode ready\n");
}

/* Starts the running task in ring 3 at `entry` on `user_stack_top`, as a 32-bit program
 * (compatibility mode) or a 64-bit one. It starts with clean registers, as on Linux: glibc's
 * _start reads %edx/%rdx as a function to run at exit (the dynamic linker's), and a leftover
 * kernel value there made every static glibc program jump into its own stack when it exited.
 * The kernel stack ring 3 comes back to (TSS rsp0) is the task's own, set by the scheduler. */
__attribute__((noreturn)) void enter_user_mode_as(uint64_t entry, uint64_t user_stack_top, int is64)
{
    uint64_t cs = is64 ? GDT_USER64_CS : GDT_USER32_CS;
    asm volatile(
        "cli\n"
        "mov %4, %%ds\n"
        "mov %4, %%es\n"
        "pushq %4\n"
        "pushq %1\n"
        "pushfq\n"
        "orq $0x200, (%%rsp)\n"
        "pushq %2\n"
        "pushq %0\n"
        "xorl %%eax, %%eax\n"
        "xorl %%ebx, %%ebx\n"
        "xorl %%ecx, %%ecx\n"
        "xorl %%edx, %%edx\n"
        "xorl %%esi, %%esi\n"
        "xorl %%edi, %%edi\n"
        "xorl %%ebp, %%ebp\n"
        "xorl %%r8d, %%r8d\n"
        "xorl %%r9d, %%r9d\n"
        "xorl %%r10d, %%r10d\n"
        "xorl %%r11d, %%r11d\n"
        "xorl %%r12d, %%r12d\n"
        "xorl %%r13d, %%r13d\n"
        "xorl %%r14d, %%r14d\n"
        "xorl %%r15d, %%r15d\n"
        "iretq\n"
        :
        : "r"(entry), "r"(user_stack_top), "r"(cs), "i"(0), "r"((uint64_t)GDT_USER_DS)
        : "memory"
    );
    __builtin_unreachable();
}

__attribute__((noreturn)) void enter_user_mode(uint32_t entry, uint32_t user_stack_top)
{
    enter_user_mode_as(entry, user_stack_top, 0);
}
