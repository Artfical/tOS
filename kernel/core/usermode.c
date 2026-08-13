#include "usermode.h"
#include "tss.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"

/* volatile: the only reads of these happen inside inline asm, which is
 * opaque to the optimizer's normal dataflow analysis -- without
 * volatile, -O2 saw sys_exit_set_jmp()'s assignments as dead stores
 * with no visible reader and dropped them (and their storage)
 * entirely, turning the direct-by-name references in
 * sys_exit_longjmp()'s asm into undefined symbols at link time. */
static volatile uint32_t exit_esp;
static volatile uint32_t exit_ebp;
static volatile uint32_t exit_ebx;
static volatile uint32_t exit_esi;
static volatile uint32_t exit_edi;
static volatile uint32_t exit_eip;

/* enter_user_mode() is __attribute__((noreturn)) and really does
 * never return the normal way (it iret's straight to ring3) -- so the
 * compiler doesn't bother preserving cmd_run()'s callee-saved
 * registers (ebp/ebx/esi/edi) across that call the way a real
 * function return would guarantee. sys_exit_longjmp() is the thing
 * that actually "returns" to cmd_run() later (once the ring3 program
 * calls SYS_EXIT), by teleporting esp/eip back to where
 * sys_exit_set_jmp() captured them -- but if it only restores esp/eip
 * and leaves ebp/ebx/esi/edi as whatever garbage they picked up deep
 * inside the syscall handler's own call chain, cmd_run()'s -O2 code
 * (which addresses spilled locals ebp-relative) reads/writes through
 * a completely unrelated pointer. Confirmed via a reproducible page
 * fault (CR2 landing exactly on the ring3 stack's upper boundary,
 * consistent with a leftover value from deep in the syscall/gfx code
 * path) the moment a real .t program (this session's video player)
 * changed the exact nested call depth at SYS_EXIT time enough for that
 * garbage to land somewhere unmapped instead of "accidentally fine" --
 * every earlier .t program was silently relying on that same
 * accident. All 4 callee-saved registers must round-trip exactly. */
void sys_exit_set_jmp(uint32_t esp, uint32_t ebp, uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t eip)
{
    exit_esp = esp;
    exit_ebp = ebp;
    exit_ebx = ebx;
    exit_esi = esi;
    exit_edi = edi;
    exit_eip = eip;
}

void __attribute__((noreturn)) sys_exit_longjmp(void)
{
    /* Reads the saved registers straight out of the global variables
     * by symbol name rather than through GCC-chosen "r"/"m" operands.
     * An operand the compiler decided to spill gets addressed relative
     * to *this function's own* current esp or ebp -- and this asm
     * block's whole job is to overwrite both of those, so any operand
     * still needed after the first instruction that touches esp or
     * ebp becomes unreachable (confirmed via two different reproducible
     * crashes: one jumping to garbage exactly at the ring3 program's
     * memory boundary, another to fully random garbage, from two
     * different attempts at getting the instruction *order* right).
     * Global symbols have fixed link-time addresses, not frame-
     * relative ones, so this sidesteps the whole class of bug instead
     * of trying to out-order it. */
    asm volatile(
        "movl exit_ebp, %%ebp\n"
        "movl exit_ebx, %%ebx\n"
        "movl exit_esi, %%esi\n"
        "movl exit_edi, %%edi\n"
        "movl exit_eip, %%eax\n"
        "movl exit_esp, %%esp\n"
        "jmp *%%eax\n"
        :
        :
        : "eax", "memory"
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
