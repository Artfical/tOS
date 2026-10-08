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

/* User address space: [PAGING_USER_START, PAGING_USER_END) belongs to the process whose directory is loaded;
 * everything else is the kernel's (identity-mapped RAM, device memory) and shared by all directories. */
#define PAGING_USER_START 0x80000000u
#define PAGING_USER_END   0xC0000000u

uint32_t *paging_kernel_dir(void);
uint32_t *paging_current_dir(void);                     /* page directory loaded in CR3 */
int  paging_map_in(uint32_t *dir, uint32_t virt, uint32_t phys, uint32_t flags);   /* 0 = ok */
/* Is [virt, virt+len) mapped user-accessible (and writable when `write`) in the loaded directory? */
int  paging_user_range_ok(uint32_t virt, uint32_t len, int write);

#endif
