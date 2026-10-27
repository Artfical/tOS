/* Copied to 0x8000 and entered there in 64-bit mode, interrupts off, with tables that map the
 * first 4 GB at their own addresses: leaves long mode (32-bit code segment 0xC0, paging off,
 * EFER.LME off), copies (or zeroes) the new kernel's segments to where they belong, and jumps to
 * it as a Multiboot loader would (eax = 0x2BADB002, ebx = the Multiboot information). Its table
 * is at 0x7000: [0] count, [4] entry, [8] Multiboot info, then entries of (destination, source or
 * 0, length). Position independent: it uses no absolute address but the table's. */
.section .text
.global kexec_tramp_start
.global kexec_tramp_end
.code64
kexec_tramp_start:
    cli
    movq $0x7ff0, %rsp
    leaq 1f(%rip), %rax
    pushq $0xC0
    pushq %rax
    lretq
.code32
1:  movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    movl %cr0, %eax
    andl $0x7fffffff, %eax          /* paging off: the CPU leaves long mode */
    movl %eax, %cr0
    movl $0xC0000080, %ecx
    rdmsr
    andl $~(1 << 8), %eax           /* EFER.LME off */
    wrmsr
    movl %cr4, %eax
    andl $~(1 << 5), %eax           /* PAE off: the next kernel starts as after a Multiboot loader */
    movl %eax, %cr4
    movl $0x7000, %ebp
    movl 0(%ebp), %ebx
    leal 16(%ebp), %edx
    cld
2:  testl %ebx, %ebx
    jz 4f
    movl 0(%edx), %edi
    movl 4(%edx), %esi
    movl 8(%edx), %ecx
    testl %esi, %esi
    jz 3f
    rep movsb
    jmp 5f
3:  xorl %eax, %eax
    rep stosb
5:  addl $12, %edx
    decl %ebx
    jmp 2b
4:  movl 8(%ebp), %ebx
    movl 4(%ebp), %ecx
    movl $0x2BADB002, %eax
    jmp *%ecx
kexec_tramp_end:
