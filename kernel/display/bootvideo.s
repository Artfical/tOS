/* bootvideo.s - links the boot animation (tools/gen_bootvideo.py) into the kernel's rodata */
    .section .rodata
    .global boot_video
boot_video:
    .incbin "assets/boot/bootvideo.bin"
