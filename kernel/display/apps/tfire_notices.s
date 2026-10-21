# THIRD_PARTY_NOTICES.html (the licences of what tOS is built from, as a page), 0-terminated, for tfire:licenses
    .section .rodata
    .global tfire_notices
tfire_notices:
    .incbin "THIRD_PARTY_NOTICES.html"
    .byte 0
    .section .note.GNU-stack,"",@progbits
