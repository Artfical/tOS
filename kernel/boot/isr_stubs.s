.intel_syntax noprefix

.macro isr_noerr num
.global isr\num
isr\num:
    push 0
    push \num
    jmp isr_common_stub
.endm

.macro isr_err num
.global isr\num
isr\num:
    push \num
    jmp isr_common_stub
.endm

.macro irq_stub num
.global irq\num
irq\num:
    push 0
    push \num + 32
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
    push 0
    push 0x80
    jmp isr_common_stub

.global isr_stub_table
isr_stub_table:
.long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
.long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
.long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
.long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.long irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
.long irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
.long syscall_stub
