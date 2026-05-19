#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define PAGE_DIR_ENTRIES 1024
#define PAGE_TABLE_ENTRIES 1024

#define PDE_PRESENT  0x001
#define PDE_WRITABLE 0x002
#define PDE_USER     0x004
#define PDE_4MB      0x080

#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004

void paging_init(void);
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);
void paging_switch(uint32_t *dir);
uint32_t *paging_create_dir(void);
void paging_destroy_dir(uint32_t *dir);
uint32_t paging_virt_to_phys(uint32_t *dir, uint32_t virt);

#endif
