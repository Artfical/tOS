#include "elf.h"
#include "memory.h"
#include "paging.h"
#include "string.h"
#include "terminal.h"
#include "ramfs.h"

#define LIBC_BASE 0x40000000

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

/* Maps and copies every PT_LOAD segment of `data` at p_vaddr + base.
 * If a PT_DYNAMIC segment exists, its (relocated) vaddr is written to
 * *dyn_vaddr; otherwise *dyn_vaddr is left at 0. */
static int elf_load_segments(void *data, elf_header_t *hdr, uint32_t base, uint32_t *dyn_vaddr)
{
    *dyn_vaddr = 0;

    for (int i = 0; i < hdr->e_phnum; i++) {
        elf_program_header_t *phdr = (elf_program_header_t *)((uint32_t)data + hdr->e_phoff + i * hdr->e_phentsize);

        if (phdr->p_type == PT_DYNAMIC) {
            *dyn_vaddr = phdr->p_vaddr + base;
            continue;
        }
        if (phdr->p_type != PT_LOAD) continue;

        uint32_t vaddr = phdr->p_vaddr + base;
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

    return 0;
}

static uint32_t dyn_get(elf_dyn_t *dyn, int32_t tag)
{
    for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag) return dyn->d_val;
    return 0;
}

/* Finds a defined (st_value != 0, st_shndx != 0) symbol named `name`
 * in a shared object's dynamic symbol table and returns its runtime
 * address (base already applied), or 0 if not found. */
static uint32_t resolve_in_lib(uint32_t symtab, uint32_t strtab, uint32_t nsyms, uint32_t lib_base, const char *name)
{
    elf_sym_t *sym = (elf_sym_t *)symtab;
    for (uint32_t i = 1; i < nsyms; i++) {
        elf_sym_t *s = &sym[i];
        if (s->st_name == 0 || s->st_shndx == 0) continue;
        const char *sname = (const char *)(strtab + s->st_name);
        if (strcmp(sname, name) == 0) return lib_base + s->st_value;
    }
    return 0;
}

static void apply_relocations(elf_rel_t *rel, uint32_t relsz, elf_sym_t *symtab, const char *strtab,
                               uint32_t exe_base, uint32_t libc_symtab, uint32_t libc_strtab,
                               uint32_t libc_nsyms, uint32_t libc_base)
{
    int count = relsz / sizeof(elf_rel_t);
    for (int i = 0; i < count; i++) {
        elf_rel_t *r = &rel[i];
        uint32_t type = ELF32_R_TYPE(r->r_info);
        uint32_t symidx = ELF32_R_SYM(r->r_info);
        uint32_t *loc = (uint32_t *)(r->r_offset + exe_base);

        uint32_t resolved = 0;
        if (symidx != 0) {
            const char *name = strtab + symtab[symidx].st_name;
            resolved = resolve_in_lib(libc_symtab, libc_strtab, libc_nsyms, libc_base, name);
        }

        switch (type) {
            case R_386_32:       *loc += resolved; break;
            case R_386_PC32:     *loc += resolved - (uint32_t)loc; break;
            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT: *loc = resolved; break;
            case R_386_RELATIVE: *loc += exe_base; break;
            default: break;
        }
    }
}

int elf_load_dynamic(void *data, uint32_t size, uint32_t *entry)
{
    (void)size;
    elf_header_t *hdr = (elf_header_t *)data;

    if (hdr->e_magic != ELF_MAGIC) {
        terminal_writestring("ELF: Invalid magic\n");
        return -1;
    }
    if (hdr->e_machine != 3) {
        terminal_writestring("ELF: Not i386\n");
        return -1;
    }

    uint32_t exe_dyn_vaddr = 0;
    if (elf_load_segments(data, hdr, 0, &exe_dyn_vaddr) != 0) return -1;
    *entry = hdr->e_entry;

    if (exe_dyn_vaddr == 0) {
        /* Statically linked (or no PT_DYNAMIC) — nothing more to do. */
        return 0;
    }

    elf_dyn_t *exe_dyn = (elf_dyn_t *)exe_dyn_vaddr;
    uint32_t exe_strtab = dyn_get(exe_dyn, DT_STRTAB);
    uint32_t exe_symtab = dyn_get(exe_dyn, DT_SYMTAB);
    uint32_t needed_off  = dyn_get(exe_dyn, DT_NEEDED);

    if (exe_strtab == 0 || exe_symtab == 0) {
        terminal_writestring("ELF: dynamic exe missing symtab/strtab\n");
        return -1;
    }
    if (needed_off == 0) {
        /* PT_DYNAMIC present but nothing needed — unusual but not fatal. */
        return 0;
    }

    const char *needed_name = (const char *)(exe_strtab + needed_off);
    char lib_path[80] = "/lib/";
    int lp = 5;
    for (int i = 0; needed_name[i] && lp < (int)sizeof(lib_path) - 1; i++) lib_path[lp++] = needed_name[i];
    lib_path[lp] = 0;

    if (!ramfs_exists(lib_path)) {
        terminal_writestring("ELF: shared library not found: ");
        terminal_writestring(lib_path);
        terminal_writestring("\n");
        return -1;
    }

    uint32_t lib_sz = ramfs_size(lib_path);
    void *libdata = malloc(lib_sz);
    if (!libdata) return -1;
    ramfs_read(lib_path, libdata, lib_sz, 0);

    elf_header_t *libhdr = (elf_header_t *)libdata;
    if (libhdr->e_magic != ELF_MAGIC || libhdr->e_type != ET_DYN) {
        terminal_writestring("ELF: shared library is not ET_DYN\n");
        free(libdata);
        return -1;
    }

    uint32_t lib_dyn_vaddr = 0;
    if (elf_load_segments(libdata, libhdr, LIBC_BASE, &lib_dyn_vaddr) != 0) {
        free(libdata);
        return -1;
    }
    if (lib_dyn_vaddr == 0) {
        terminal_writestring("ELF: shared library missing PT_DYNAMIC\n");
        free(libdata);
        return -1;
    }

    elf_dyn_t *lib_dyn = (elf_dyn_t *)lib_dyn_vaddr;
    uint32_t lib_strtab = dyn_get(lib_dyn, DT_STRTAB) + LIBC_BASE;
    uint32_t lib_symtab = dyn_get(lib_dyn, DT_SYMTAB) + LIBC_BASE;
    uint32_t lib_hash = dyn_get(lib_dyn, DT_HASH);
    if (lib_hash == 0) {
        terminal_writestring("ELF: shared library missing DT_HASH (link with --hash-style=sysv)\n");
        free(libdata);
        return -1;
    }
    uint32_t lib_nsyms = ((uint32_t *)(lib_hash + LIBC_BASE))[1]; /* nchain == symbol count */

    /* Relocate the library's own internal pointers first (R_386_RELATIVE
     * entries produced by -fPIC for its own global data), then resolve
     * its own intra-library PLT/GOT calls (e.g. exit() calling _exit()
     * through its own .rel.plt) against its own symbol table — -fPIC
     * code doesn't assume it will be the only shared object loaded, so
     * even same-library calls go through the GOT/PLT indirection. */
    uint32_t lib_rel = dyn_get(lib_dyn, DT_REL);
    uint32_t lib_relsz = dyn_get(lib_dyn, DT_RELSZ);
    if (lib_rel && lib_relsz) {
        apply_relocations((elf_rel_t *)(lib_rel + LIBC_BASE), lib_relsz,
                           (elf_sym_t *)lib_symtab, (const char *)lib_strtab,
                           LIBC_BASE, lib_symtab, lib_strtab, lib_nsyms, LIBC_BASE);
    }
    uint32_t lib_jmprel = dyn_get(lib_dyn, DT_JMPREL);
    uint32_t lib_pltrelsz = dyn_get(lib_dyn, DT_PLTRELSZ);
    if (lib_jmprel && lib_pltrelsz) {
        apply_relocations((elf_rel_t *)(lib_jmprel + LIBC_BASE), lib_pltrelsz,
                           (elf_sym_t *)lib_symtab, (const char *)lib_strtab,
                           LIBC_BASE, lib_symtab, lib_strtab, lib_nsyms, LIBC_BASE);
    }

    /* Now resolve the main executable's imported symbols against the
     * library's dynamic symbol table and patch .rel.dyn / .rel.plt. */
    uint32_t exe_rel = dyn_get(exe_dyn, DT_REL);
    uint32_t exe_relsz = dyn_get(exe_dyn, DT_RELSZ);
    if (exe_rel && exe_relsz) {
        apply_relocations((elf_rel_t *)exe_rel, exe_relsz, (elf_sym_t *)exe_symtab, (const char *)exe_strtab,
                           0, lib_symtab, lib_strtab, lib_nsyms, LIBC_BASE);
    }

    uint32_t exe_jmprel = dyn_get(exe_dyn, DT_JMPREL);
    uint32_t exe_pltrelsz = dyn_get(exe_dyn, DT_PLTRELSZ);
    if (exe_jmprel && exe_pltrelsz) {
        apply_relocations((elf_rel_t *)exe_jmprel, exe_pltrelsz, (elf_sym_t *)exe_symtab, (const char *)exe_strtab,
                           0, lib_symtab, lib_strtab, lib_nsyms, LIBC_BASE);
    }

    free(libdata);
    return 0;
}
