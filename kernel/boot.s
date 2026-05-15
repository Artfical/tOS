.intel_syntax noprefix

#include "multiboot2.h"

.section .multiboot, "a"
.align 8

.long MULTIBOOT2_HEADER_MAGIC
.long MULTIBOOT2_HEADER_ARCHITECTURE_I386
.long multiboot_header_end - multiboot_header_start
.long -(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_HEADER_ARCHITECTURE_I386 + (multiboot_header_end - multiboot_header_start))

multiboot_header_start:
.short MULTIBOOT2_HEADER_TAG_INFORMATION_REQUEST
.short 0
.long information_request_end - information_request_start
information_request_start:
.long MULTIBOOT2_TAG_TYPE_MODULE
information_request_end:

.short MULTIBOOT2_HEADER_TAG_MODULE_ALIGN
.short 0
.long 8

.short MULTIBOOT2_HEADER_TAG_TERMINAL
.short 0
.long 8

.short MULTIBOOT2_HEADER_TAG_END
.short 0
.long 8
multiboot_header_end:

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
