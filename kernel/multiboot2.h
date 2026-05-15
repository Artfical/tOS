#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#define MULTIBOOT2_HEADER_MAGIC 0xE85250D6
#define MULTIBOOT2_HEADER_ARCHITECTURE_I386 0

#define MULTIBOOT2_HEADER_TAG_END 0
#define MULTIBOOT2_HEADER_TAG_INFORMATION_REQUEST 1
#define MULTIBOOT2_HEADER_TAG_MODULE_ALIGN 6
#define MULTIBOOT2_HEADER_TAG_TERMINAL 7

#define MULTIBOOT2_TAG_TYPE_MODULE 3
#define MULTIBOOT2_TAG_TYPE_MMAP 6

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

#ifndef __ASSEMBLER__

#include <stdint.h>

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[];
};

struct multiboot2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
};

struct multiboot2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot2_mmap_entry entries[];
};

#endif
#endif
