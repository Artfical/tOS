/* Copied to 0x8000 and run there, identity mapped, with interrupts off: turns paging off, copies
 * (or zeroes) the new kernel's segments to where they belong, and jumps to it as a Multiboot
 * loader would (eax = 0x2BADB002, ebx = the Multiboot information). Its table is at 0x7000:
 * [0] count, [4] entry, [8] Multiboot info, then entries of (destination, source or 0, length).
 * Position independent: it uses no absolute address but the table's. */
.section .text
.global kexec_tramp_start
.global kexec_tramp_end
.code32
kexec_tramp_start:
    cli
    movl $0x7ff0, %esp
    movl %cr0, %eax
    andl $0x7fffffff, %eax
    movl %eax, %cr0
    jmp 1f
1:  movl $0x7000, %ebp
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
