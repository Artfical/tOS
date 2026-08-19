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
    /* "main" runs the full depth of every syscall a ring3 .t program
     * makes, including nested network-protocol-handler call chains
     * (net_poll() -> nic_poll() -> whichever driver is actually
     * detected) -- and the exact stack depth needed there depends on
     * which NIC driver is live, which differs by hypervisor/hardware.
     * Bumped from 128KB (itself an earlier defensive bump from 16KB)
     * while chasing a deterministic, real-hardware-only crash
     * (instant GPF reading a kernel global from inside timer_handler)
     * that never reproduces under QEMU -- consistent with a marginal
     * stack overflow into adjacent kernel .data on a code path QEMU's
     * NIC driver happens to need less stack for than a real machine's
     * or VMware's does. Not confirmed as the fix, but a reasonable,
     * cheap safety margin regardless. */
    .skip 262144
stack_top:
