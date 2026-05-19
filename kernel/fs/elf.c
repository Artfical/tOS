#include "elf.h"
#include "memory.h"
#include "paging.h"
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
            uint32_t vaddr = phdr->p_vaddr;
            uint32_t memsz = phdr->p_memsz;
            uint32_t filesz = phdr->p_filesz;
            void *src = (void *)((uint32_t)data + phdr->p_offset);
            uint32_t flags = PTE_USER | PTE_WRITABLE;

            uint32_t page_start = vaddr & ~0xFFF;
            uint32_t page_end = (vaddr + memsz + 0xFFF) & ~0xFFF;

            for (uint32_t page = page_start; page < page_end; page += 0x1000) {
                if (!paging_virt_to_phys(NULL, page)) {
                    uint32_t phys = alloc_physical_page();
                    if (!phys) {
                        terminal_writestring("ELF: OOM loading segment\n");
                        return -1;
                    }
                    paging_map(page, phys, flags);
                }
            }

            memcpy((void *)vaddr, src, filesz);
            if (memsz > filesz) {
                memset((void *)(vaddr + filesz), 0, memsz - filesz);
            }
        }
    }

    return 0;
}
