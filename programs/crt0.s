.intel_syntax noprefix
.global _start
.global __main
.extern main
.extern exit

.section .text

_start:
    xor ebp, ebp
    mov edx, esp
    and esp, -16

    mov ecx, [edx]
    lea ebx, [edx + 4]
    lea eax, [ebx + ecx*4 + 4]

    push eax
    push ebx
    push ecx

    call main

    push eax
    call exit

    hlt

__main:
    ret
