#include "usermode.h"
#include "tss.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"

static uint32_t exit_esp;
static uint32_t exit_eip;

void sys_exit_set_jmp(uint32_t esp, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t eip)
{
    (void)ebp; (void)ebx; (void)esi; (void)edi;
    exit_esp = esp;
    exit_eip = eip;
}

void __attribute__((noreturn)) sys_exit_longjmp(void)
{
    asm volatile(
        "mov %0, %%esp\n"
        "jmp *%1\n"
        :
        : "r"(exit_esp), "r"(exit_eip)
    );
    __builtin_unreachable();
}

#define USER_STACK_PAGES 16 /* 64 KB — enough headroom for nested libc calls */

void usermode_init(void)
{
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        uint32_t page = alloc_physical_page();
        if (!page) {
            terminal_writestring("usermode: failed to allocate stack page\n");
            return;
        }
        paging_map(USER_STACK_TOP - i * 4096, page, PTE_USER | PTE_WRITABLE);
    }
    terminal_writestring("[OK] User mode stack ready\n");
}

__attribute__((noreturn)) void enter_user_mode(uint32_t entry, uint32_t user_stack_top)
{
    uint32_t kernel_esp;
    asm volatile("mov %%esp, %0" : "=r"(kernel_esp));
    tss_set_kernel_stack(kernel_esp);

    asm volatile(
        "cli\n"
        "pushl %0\n"
        "pushl %1\n"
        "pushf\n"
        "popl %%eax\n"
        "orl $0x200, %%eax\n"
        "pushl %%eax\n"
        "pushl %2\n"
        "pushl %3\n"
        "iret\n"
        :
        : "i"(0x23),
          "r"(user_stack_top),
          "i"(0x1B),
          "r"(entry)
        : "eax", "memory"
    );
    __builtin_unreachable();
}
