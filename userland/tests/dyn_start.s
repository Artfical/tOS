/* Minimal _start for dynamically-linked tOS test binaries. Unlike the
 * static-binary crt0.s, this never dereferences the initial ESP (the
 * kernel maps only a handful of guard pages below USER_STACK_TOP, so
 * reading *exactly* at the top address page-faults) — it just calls
 * main() then exit() with its return value. */
.intel_syntax noprefix
.global _start
.extern main
.extern exit

.section .text
_start:
    xor ebp, ebp
    and esp, -16
    call main
    push eax
    call exit
    hlt
