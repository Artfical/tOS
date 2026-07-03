#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define ELF_MAGIC 0x464C457F

typedef struct {
    uint32_t e_magic;
    uint8_t e_ident[12];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf_header_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf_program_header_t;

#define ET_EXEC 2
#define ET_DYN  3

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

/* Elf32_Dyn */
typedef struct {
    int32_t  d_tag;
    uint32_t d_val;
} __attribute__((packed)) elf_dyn_t;

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_JMPREL   23

/* Elf32_Sym */
typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} __attribute__((packed)) elf_sym_t;

/* Elf32_Rel */
typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} __attribute__((packed)) elf_rel_t;

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)

#define R_386_32       1
#define R_386_PC32     2
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8

int elf_load(void *data, uint32_t *entry);

/* Dynamic-linking loader: resolves a single DT_NEEDED shared object
 * (looked up by soname in ramfs at /lib/<soname>) and applies its
 * relocations against the main executable. Returns 0 and *entry on
 * success. Main executable must be non-PIE (ET_EXEC, fixed vaddrs);
 * the shared object is loaded at a fixed base and relocated. */
int elf_load_dynamic(void *data, uint32_t size, uint32_t *entry);

#endif
