/* wolf_jmp_buf layout (see wolf_jmp.h): ebx=+0, esi=+4, edi=+8,
 * ebp=+12, esp=+16, eip=+20 -- the i386 SysV callee-saved registers
 * plus the return address, exactly enough to unwind the C call stack
 * safely (see wolf_jmp.h's comment for why this exists at all). */
.text

.global wolf_setjmp
.type wolf_setjmp, @function
wolf_setjmp:
    mov 4(%esp), %eax
    mov %ebx, 0(%eax)
    mov %esi, 4(%eax)
    mov %edi, 8(%eax)
    mov %ebp, 12(%eax)
    /* Save the caller's esp as it will be right after this function
     * returns via `ret` (i.e. post return-address-pop), not the raw
     * entry esp -- wolf_longjmp below resumes via a bare `jmp` to the
     * saved eip, which (unlike `ret`) never pops that return address
     * off the stack itself. Saving the raw entry esp here left the
     * jmp-restored stack 4 bytes short of where the resumed code
     * actually expects it, corrupting every subsequent stack-relative
     * access in the caller. */
    lea 4(%esp), %ecx
    mov %ecx, 16(%eax)
    mov (%esp), %ecx
    mov %ecx, 20(%eax)
    xor %eax, %eax
    ret

.global wolf_longjmp
.type wolf_longjmp, @function
wolf_longjmp:
    mov 4(%esp), %ecx
    mov 8(%esp), %eax
    mov 0(%ecx), %ebx
    mov 4(%ecx), %esi
    mov 8(%ecx), %edi
    mov 12(%ecx), %ebp
    mov 16(%ecx), %esp
    jmp *20(%ecx)
