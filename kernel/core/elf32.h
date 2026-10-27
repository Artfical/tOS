#ifndef ELF32_H
#define ELF32_H

#include <stdint.h>

/* Load a static, little-endian ELF32/i386 executable into an already-created
 * user address space. Dynamic interpreters and PT_DYNAMIC images are rejected. */
int elf32_load_static(const char *path, uint32_t file_size, uint32_t *pgdir,
                      uint32_t *entry_out, uint32_t *brk_base_out,
                      uint32_t *phdr_out, uint32_t *phnum_out);

#endif
