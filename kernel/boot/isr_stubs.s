/* Interrupt entry: every vector builds the same frame (registers_t in isr.h) -- an error code or 0,
 * the vector, the general registers, the segment selectors -- and hands it to C. The timer is the
 * one that may return a different frame: timer_handler gets the interrupted task's frame and
 * returns the one of the task to run next, which is how tasks switch. */
.code64
.text

.macro SAVE_REGS
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq $0                    /* esp/rsp_unused: the frame's own rsp is the CPU's */
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movl %ds, %eax
    pushq %rax
    movl %es, %eax
    pushq %rax
    movl %fs, %eax
    pushq %rax
    movl %gs, %eax
    pushq %rax
    cld
.endm

/* fs is not reloaded: a 64-bit program's thread pointer is the FS base, which the scheduler keeps
 * per task, and loading a selector could clear it. gs is (32-bit programs' TLS lives there). */
.macro RESTORE_REGS
    popq %rax
    movl %eax, %gs
    addq $8, %rsp
    popq %rax
    movl %eax, %es
    popq %rax
    movl %eax, %ds
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    addq $8, %rsp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp              /* vector and error code */
.endm

.macro isr_noerr num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common_stub
.endm

.macro isr_err num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common_stub
.endm

.macro irq_stub num
.global irq\num
irq\num:
    pushq $0
    pushq $(\num + 32)
    jmp irq_common_stub
.endm

isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err 8
isr_noerr 9
isr_err 10
isr_err 11
isr_err 12
isr_err 13
isr_err 14
isr_noerr 15
isr_noerr 16
isr_err 17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_noerr 21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_noerr 29
isr_err 30
isr_noerr 31

irq_stub 0
irq_stub 1
irq_stub 2
irq_stub 3
irq_stub 4
irq_stub 5
irq_stub 6
irq_stub 7
irq_stub 8
irq_stub 9
irq_stub 10
irq_stub 11
irq_stub 12
irq_stub 13
irq_stub 14
irq_stub 15

.global syscall_stub
syscall_stub:
    pushq $0
    pushq $0x80
    jmp isr_common_stub

/* The CPU leaves rsp 16-byte aligned before its five pushes; with ours the frame is 216 bytes,
 * so rsp is 8 off. 520 more bytes hold the interrupted FPU/SSE state (fxsave64 needs 16-byte
 * alignment) and leave rsp aligned for the call, as C expects. */
.macro CALL_C_SAVING_FPU handler
    movq %rsp, %rdi
    subq $520, %rsp
    fxsave64 (%rsp)
    call \handler
    fxrstor64 (%rsp)
    addq $520, %rsp
.endm

.global isr_common_stub
isr_common_stub:
    SAVE_REGS
    CALL_C_SAVING_FPU isr_handler
    RESTORE_REGS
    iretq

.global irq_common_stub
irq_common_stub:
    SAVE_REGS
    CALL_C_SAVING_FPU irq_handler
    RESTORE_REGS
    iretq

/* no FPU save here: timer_handler saves the interrupted task's and loads the next one's */
.global timer_irq_stub
timer_irq_stub:
    pushq $0
    pushq $32
    SAVE_REGS
    movq %rsp, %rdi
    subq $8, %rsp
    call timer_handler
    movq %rax, %rsp
    RESTORE_REGS
    iretq

.section .rodata
.global isr_stub_table
isr_stub_table:
.quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
.quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
.quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
.quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
.quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
.quad syscall_stub
