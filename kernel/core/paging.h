#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 4096

#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PTE_PCD      0x010
#define PTE_HUGE     0x080
#define PTE_NX       (1ull << 63)
#define PDE_PRESENT  PTE_PRESENT
#define PDE_WRITABLE PTE_WRITABLE
#define PDE_USER     PTE_USER

/* The kernel's half of every address space:
 *   0xFFFF800000000000 + physical   all of physical memory and the device memory below 4 GB
 *   0xFFFFFFFF80000000 + physical   the kernel image (and the rest of the first 2 GB)
 * The lower half belongs to the process whose tables are loaded: 32-bit programs live between
 * 0x80000000 and 0xC0000000, 64-bit ones anywhere below PAGING_USER_END. */
#define DIRECT_MAP_BASE 0xFFFF800000000000ull
#define KERNEL_VMA      0xFFFFFFFF80000000ull
#define PAGING_USER_START 0x1000ull
#define PAGING_USER_END   0x0000800000000000ull

static inline void *P2V(uint64_t phys) { return (void *)(phys + DIRECT_MAP_BASE); }
static inline uint64_t V2P(const volatile void *virt)
{
    uint64_t v = (uint64_t)virt;
    return v >= KERNEL_VMA ? v - KERNEL_VMA : v - DIRECT_MAP_BASE;
}

void paging_init(void);
/* Kernel mappings of device memory: physical memory below 4 GB is always mapped (P2V), so these
 * only exist for the callers that used to identity-map it; they map nothing. */
void paging_map(uint64_t virt, uint64_t phys, uint32_t flags);
void paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
void paging_switch(uint64_t *dir);
uint64_t *paging_create_dir(void);
void paging_destroy_dir(uint64_t *dir);
uint64_t paging_virt_to_phys(uint64_t *dir, uint64_t virt);

uint64_t *paging_kernel_dir(void);
uint64_t *paging_current_dir(void);                     /* the tables loaded in CR3 */
int  paging_map_in(uint64_t *dir, uint64_t virt, uint64_t phys, uint64_t flags);   /* 0 = ok */
int  paging_unmap_in(uint64_t *dir, uint64_t virt, uint64_t *phys_out);
int  paging_protect_in(uint64_t *dir, uint64_t virt, uint64_t flags);
/* Is [virt, virt+len) mapped user-accessible (and writable when `write`) in the loaded tables? */
int  paging_user_range_ok(uint64_t virt, uint64_t len, int write);

#endif
