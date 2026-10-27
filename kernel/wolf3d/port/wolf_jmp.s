/* wolf_jmp_buf layout (see wolf_jmp.h): rbx, rbp, r12, r13, r14, r15, rsp, rip -- the x86-64
 * SysV callee-saved registers plus where to resume, exactly enough to unwind the C call stack
 * (see wolf_jmp.h's comment for why this exists at all). rsp is saved as it is after
 * wolf_setjmp returns (past the return address), since wolf_longjmp resumes with a jmp. */
.text

.global wolf_setjmp
.type wolf_setjmp, @function
wolf_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

.global wolf_longjmp
.type wolf_longjmp, @function
wolf_longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    incl %eax
1:  movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmp *56(%rdi)
