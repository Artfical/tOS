/* Loaded at 2 MB physical; everything after the 32-bit start runs at 0xFFFFFFFF80000000 + that. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 2M;
    .boot : { *(.multiboot) *(.boot32) }
    .boot.bss (NOLOAD) : ALIGN(4K) { *(.boot32.bss) }

    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) { *(.text .text.*) }
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) { *(.rodata .rodata.*) }
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) { *(.data .data.*) }
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
    }
    end = .;
    /DISCARD/ : { *(.eh_frame) *(.note.gnu.property) *(.comment) }
}
