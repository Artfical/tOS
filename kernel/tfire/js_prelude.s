# tFire's DOM in JavaScript (prelude.js), as a 0-terminated string for js.c
    .section .rodata
    .global tf_prelude
tf_prelude:
    .incbin "kernel/tfire/prelude.js"
    .byte 0
    .section .note.GNU-stack,"",@progbits
