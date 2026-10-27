/* tOS x86-64 start: a Multiboot (1) loader (QEMU -kernel, GRUB, Software Update's kexec) starts
 * this 32-bit code at its physical address with eax = 0x2BADB002, ebx = the Multiboot info.
 * It maps physical memory twice -- identity for the switch, and at the kernel's place in the
 * top 2 GB (0xFFFFFFFF80000000 + physical) plus the direct map (0xFFFF800000000000 + physical)
 * -- turns on long mode and jumps to the 64-bit kernel in the higher half. */
.set KERNEL_VMA, 0xFFFFFFFF80000000
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long -(MB_MAGIC + MB_FLAGS)

.section .boot32, "ax"
.code32
.global start
start:
    cli
    movl %eax, %edi                 /* Multiboot magic and info, for kernel_main */
    movl %ebx, %esi
    movl %ebx, boot_mbinfo
    movl $boot_stack_top, %esp

    /* a 64-bit processor? (CPUID 0x80000001, EDX bit 29: long mode) */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_long_mode

    /* 0-4 GB with 2 MB pages: four page directories, filled in a loop */
    movl $boot_pd, %ebx
    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax                 /* present, writable, 2 MB */
    movl %eax, (%ebx,%ecx,8)
    movl %ecx, %eax
    shrl $11, %eax                  /* the upper 32 bits: ecx * 2 MB >> 32 */
    movl %eax, 4(%ebx,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    /* boot_pdpt_low: 0-4 GB (identity and direct map); boot_pdpt_high[510..511]: 0-2 GB */
    movl $boot_pdpt_low, %ebx
    movl $boot_pd + 0x0003, 0(%ebx)
    movl $boot_pd + 0x1003, 8(%ebx)
    movl $boot_pd + 0x2003, 16(%ebx)
    movl $boot_pd + 0x3003, 24(%ebx)
    movl $boot_pdpt_high, %ebx
    movl $boot_pd + 0x0003, 510*8(%ebx)
    movl $boot_pd + 0x1003, 511*8(%ebx)
    movl $boot_pml4, %ebx
    movl $boot_pdpt_low + 3, 0(%ebx)          /* identity, for the jump */
    movl $boot_pdpt_low + 3, 256*8(%ebx)      /* direct map 0xFFFF800000000000 */
    movl $boot_pdpt_high + 3, 511*8(%ebx)     /* the kernel 0xFFFFFFFF80000000 */

    movl %cr4, %eax
    orl $(1 << 5) | (1 << 9) | (1 << 10), %eax    /* PAE, OSFXSR, OSXMMEXCPT */
    movl %eax, %cr4
    movl $boot_pml4, %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx          /* EFER: long mode, no-execute */
    rdmsr
    orl $(1 << 8) | (1 << 11), %eax
    wrmsr
    movl %cr0, %eax
    andl $~(1 << 2), %eax           /* FPU/SSE for the kernel: no emulation, monitor */
    orl $(1 << 31) | (1 << 1) | (1 << 16), %eax   /* paging, MP, write protect */
    movl %eax, %cr0

    lgdt boot_gdtr
    ljmp $0x08, $start64_low

.code64
start64_low:
    movabsq $start64_high, %rax
    jmp *%rax

/* Not a 64-bit processor: say so on the screen and the serial port. Started by an older tOS's
 * Software Update (no boot loader name in the Multiboot information, bit 9), restart after a
 * while: the older build's boot chain stops trying this one after two attempts and starts itself
 * again. Started from a CD or a boot loader, wait. */
.code32
no_long_mode:
    movw $0x3D4, %dx                /* no text cursor */
    movb $0x0A, %al
    outb %al, %dx
    incw %dx
    movb $0x20, %al
    outb %al, %dx
    movl $0xB8000, %edi
    movl $(80 * 25), %ecx
    movl $0x4F204F20, %eax
    rep stosl
    movl $no64_msg, %esi
    movl $(0xB8000 + 160 * 10 + 2 * 4), %edi
1:  lodsb
    testb %al, %al
    jz 3f
    cmpb $10, %al
    jne 2f
    movw $0x3F8, %dx                /* the serial port gets the line break too */
    movb $13, %al
    outb %al, %dx
    movb $10, %al
    outb %al, %dx
    addl $160, %edi                 /* next line, same column (lines are 80 characters) */
    movl %edi, %eax
    subl $0xB8000, %eax
    xorl %edx, %edx
    movl $160, %ecx
    divl %ecx
    imull $160, %eax, %eax
    leal 0xB8000 + 8(%eax), %edi
    jmp 1b
2:  movb %al, (%edi)
    movb $0x4F, 1(%edi)
    addl $2, %edi
    pushl %eax
    movw $0x3FD, %dx                /* and to COM1 */
4:  inb %dx, %al
    testb $0x20, %al
    jz 4b
    popl %eax
    movw $0x3F8, %dx
    outb %al, %dx
    jmp 1b
3:  movl boot_mbinfo, %eax
    testl %eax, %eax
    jz 6f
    testl $(1 << 9), (%eax)
    jnz 6f
    movl $300000000, %ecx           /* a while (some seconds), then the keyboard controller resets */
5:  pause
    loop 5b
    movb $0xFE, %al
    outb %al, $0x64
6:  cli
    hlt
    jmp 6b

no64_msg:
    .ascii "This version of tOS is not compatible with your computer.\n"
    .ascii "\n"
    .ascii "tOS now needs a 64-bit (x86-64) processor, and this one is 32-bit.\n"
    .ascii "\n"
    .ascii "If this came from Software Update, the computer restarts by itself\n"
    .ascii "and goes back to the tOS it had before.\n"
    .byte 0

/* the GDT for the switch; the kernel loads its own */
.align 16
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF        /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF        /* 0x10: data */
boot_gdtr:
    .word boot_gdtr - boot_gdt - 1
    .long boot_gdt

.section .text
.code64
start64_high:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movabsq $stack_top, %rsp
    xorq %rbp, %rbp
    pushq $0
    popfq
    /* edi = magic, esi = Multiboot info (physical): kernel_main(uint32_t magic, uint32_t info) */
    movl %edi, %edi
    movl %esi, %esi
    call kernel_main
    cli
2:  hlt
    jmp 2b

.section .boot32.bss, "aw", @nobits
.align 4096
boot_pml4:      .skip 4096
boot_pdpt_low:  .skip 4096
boot_pdpt_high: .skip 4096
boot_pd:        .skip 4096 * 4
boot_stack:     .skip 4096
boot_stack_top:
boot_mbinfo:    .skip 4

.section .bss
.align 16
stack_bottom:
    .skip 262144
.global stack_top
stack_top:
