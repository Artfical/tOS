#ifndef KEXEC_H
#define KEXEC_H
#include <stdint.h>

/* the memory the bootloader reported, kept for the kernel started next */
void kexec_set_memory(uint32_t mem_lower, uint32_t mem_upper);
/* Starts another tOS kernel (an ELF image) with `initrd` as its module, the way the bootloader
 * would have. Returns only when the image can't be loaded; it never comes back otherwise. */
void kexec_boot(const uint8_t *elf, uint32_t elf_len, const uint8_t *initrd, uint32_t initrd_len);
#endif
