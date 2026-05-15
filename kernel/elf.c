#include "elf.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"

int elf_load(void *data, uint32_t *entry)
{
    elf_header_t *header = (elf_header_t *)data;

    if (header->e_magic != ELF_MAGIC) {
        terminal_writestring("ELF: Invalid magic\n");
        return -1;
    }
    if (header->e_machine != 3) {
        terminal_writestring("ELF: Not i386\n");
        return -1;
    }

    *entry = header->e_entry;

    for (int i = 0; i < header->e_phnum; i++) {
        elf_program_header_t *phdr = (elf_program_header_t *)((uint32_t)data + header->e_phoff + i * header->e_phentsize);

        if (phdr->p_type == PT_LOAD) {
            void *dest = (void *)phdr->p_vaddr;
            uint32_t memsz = phdr->p_memsz;
            uint32_t filesz = phdr->p_filesz;
            void *src = (void *)((uint32_t)data + phdr->p_offset);

            memcpy(dest, src, filesz);
            if (memsz > filesz) {
                memset((void *)(phdr->p_vaddr + filesz), 0, memsz - filesz);
            }
        }
    }

    return 0;
}
