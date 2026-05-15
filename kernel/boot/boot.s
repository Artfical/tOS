.intel_syntax noprefix

.section .multiboot, "a"
.align 4

.long 0x1BADB002
.long 0x00000003
.long -(0x1BADB002 + 0x00000003)

.section .text, "ax"
.global start
.type start, @function
start:
    cli
    mov esp, offset stack_top
    push 0
    popf
    push ebx
    push eax
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang

.section .bss, "aw"
.align 16
stack_bottom:
    .skip 16384
stack_top:
