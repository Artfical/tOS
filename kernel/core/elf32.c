#include "elf32.h"
#include "fsbridge.h"
#include "memory.h"
#include "paging.h"
#include "proc.h"
#include "string.h"
#include "usermode.h"

#define ELF32_MAX_PHDRS 64
#define ELF32_ET_EXEC 2
#define ELF32_EM_386 3
#define ELF32_EV_CURRENT 1
#define ELF32_PT_LOAD 1
#define ELF32_PT_DYNAMIC 2
#define ELF32_PT_INTERP 3
#define ELF32_PF_X 1

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf32_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} elf32_phdr_t;

static int elf32_read_exact(const char *path, uint32_t file_size,
                            uint32_t offset, void *dst, uint32_t size)
{
    if (offset > file_size || size > file_size - offset) return -1;
    return fsbridge_read(path, dst, size, offset) == (int)size ? 0 : -1;
}

static int elf32_map_page(uint32_t *pgdir, uint32_t page)
{
    if (paging_virt_to_phys(pgdir, page) != 0) return 0;

    uint32_t phys = alloc_physical_page();
    if (!phys) return -1;
    memset((void *)phys, 0, PAGE_SIZE);
    if (paging_map_in(pgdir, page, phys, PTE_USER | PTE_WRITABLE) != 0) {
        free_physical_page(phys);
        return -1;
    }
    return 0;
}

int elf32_load_static(const char *path, uint32_t file_size, uint32_t *pgdir,
                      uint32_t *entry_out, uint32_t *brk_base_out,
                      uint32_t *phdr_out, uint32_t *phnum_out)
{
    elf32_ehdr_t eh;
    elf32_phdr_t phdrs[ELF32_MAX_PHDRS];
    uint32_t max_mem_end = 0;
    uint32_t phdr_addr = 0;
    int entry_is_executable = 0;
    int load_count = 0;

    if (!path || !pgdir || !entry_out || !brk_base_out || !phdr_out || !phnum_out ||
        file_size < sizeof(eh)) return -1;
    if (elf32_read_exact(path, file_size, 0, &eh, sizeof(eh)) != 0) return -1;
    if (eh.ident[0] != 0x7F || eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F' ||
        eh.ident[4] != 1 || eh.ident[5] != 1 || eh.ident[6] != ELF32_EV_CURRENT ||
        eh.type != ELF32_ET_EXEC || eh.machine != ELF32_EM_386 || eh.version != ELF32_EV_CURRENT ||
        eh.ehsize != sizeof(eh) || eh.phentsize != sizeof(elf32_phdr_t) ||
        eh.phnum == 0 || eh.phnum > ELF32_MAX_PHDRS) return -1;

    uint64_t phdr_end = (uint64_t)eh.phoff + (uint64_t)eh.phnum * sizeof(elf32_phdr_t);
    if (phdr_end > file_size ||
        elf32_read_exact(path, file_size, eh.phoff, phdrs,
                         (uint32_t)eh.phnum * sizeof(elf32_phdr_t)) != 0) return -1;

    /* Validate the whole image before allocating pages. This first loader
     * intentionally accepts only static ET_EXEC files, not ld.so images. */
    for (uint32_t i = 0; i < eh.phnum; i++) {
        elf32_phdr_t *ph = &phdrs[i];
        if (ph->type == ELF32_PT_INTERP || ph->type == ELF32_PT_DYNAMIC) return -1;
        if (ph->type != ELF32_PT_LOAD) continue;
        if (ph->memsz < ph->filesz) return -1;
        if ((uint64_t)ph->offset + ph->filesz > file_size) return -1;
        if (ph->memsz == 0) continue;

        uint32_t end = ph->vaddr + ph->memsz;
        if (end < ph->vaddr || ph->vaddr < USER_CODE_BASE || end > PROC_BRK_LIMIT) return -1;
        if (ph->align > 1 && ((ph->align & (ph->align - 1)) != 0 ||
                              ((ph->vaddr - ph->offset) & (ph->align - 1)) != 0)) return -1;

        if ((ph->flags & ELF32_PF_X) && eh.entry >= ph->vaddr && eh.entry < end)
            entry_is_executable = 1;
        if (eh.phoff >= ph->offset && phdr_end <= (uint64_t)ph->offset + ph->filesz)
            phdr_addr = ph->vaddr + (eh.phoff - ph->offset);
        if (end > max_mem_end) max_mem_end = end;
        load_count++;
    }
    if (load_count == 0 || !entry_is_executable) return -1;

    /* Map and zero every PT_LOAD page first, including each segment's BSS. */
    for (uint32_t i = 0; i < eh.phnum; i++) {
        elf32_phdr_t *ph = &phdrs[i];
        if (ph->type != ELF32_PT_LOAD || ph->memsz == 0) continue;
        uint32_t end = ph->vaddr + ph->memsz;
        uint32_t page = ph->vaddr & ~(PAGE_SIZE - 1u);
        uint32_t page_end = (end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
        if (page_end < end) return -1;
        for (; page < page_end; page += PAGE_SIZE)
            if (elf32_map_page(pgdir, page) != 0) return -2;
    }

    /* Copy each file-backed range through a page-sized kernel buffer. */
    uint8_t chunk_buf[PAGE_SIZE];
    for (uint32_t i = 0; i < eh.phnum; i++) {
        elf32_phdr_t *ph = &phdrs[i];
        uint32_t va = ph->vaddr;
        uint32_t offset = ph->offset;
        uint32_t remaining = ph->filesz;
        if (ph->type != ELF32_PT_LOAD) continue;
        while (remaining) {
            uint32_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1u));
            uint32_t n = remaining < page_left ? remaining : page_left;
            if (elf32_read_exact(path, file_size, offset, chunk_buf, n) != 0) return -1;
            uint32_t phys = paging_virt_to_phys(pgdir, va);
            if (!phys) return -1;
            memcpy((void *)phys, chunk_buf, n);
            va += n;
            offset += n;
            remaining -= n;
        }
    }

    *entry_out = eh.entry;
    *brk_base_out = (max_mem_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    *phdr_out = phdr_addr;
    *phnum_out = eh.phnum;
    return *brk_base_out <= PROC_BRK_LIMIT ? 0 : -1;
}
