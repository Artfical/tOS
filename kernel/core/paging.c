#include "paging.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"

/* Four-level tables (PML4 -> PDPT -> PD -> PT). Every table is a physical page reached through
 * the direct map; entries hold physical addresses. */
#define ADDR_MASK 0x000FFFFFFFFFF000ull

static uint64_t *kernel_pml4;

static uint64_t *table(uint64_t entry) { return (uint64_t *)P2V(entry & ADDR_MASK); }

static uint64_t *new_table(void)
{
    uint32_t phys = alloc_physical_page();
    if (!phys) return 0;
    uint64_t *t = (uint64_t *)P2V(phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

uint64_t *paging_kernel_dir(void) { return kernel_pml4; }

uint64_t *paging_current_dir(void)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)P2V(cr3 & ADDR_MASK);
}

void paging_init(void)
{
    kernel_pml4 = new_table();

    /* the direct map: 2 MB pages over physical memory and at least the first 4 GB (devices) */
    uint64_t total = (uint64_t)get_total_pages() * PAGE_SIZE;
    if (total < 0x100000000ull) total = 0x100000000ull;
    uint64_t *pdpt_direct = new_table();
    kernel_pml4[256] = V2P(pdpt_direct) | PTE_PRESENT | PTE_WRITABLE;
    uint64_t *first_pds[2] = { 0, 0 };
    for (uint64_t gb = 0; gb * 0x40000000ull < total && gb < 512; gb++) {
        uint64_t *pd = new_table();
        for (int i = 0; i < 512; i++)
            pd[i] = (gb * 0x40000000ull + (uint64_t)i * 0x200000ull) | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        pdpt_direct[gb] = V2P(pd) | PTE_PRESENT | PTE_WRITABLE;
        if (gb < 2) first_pds[gb] = pd;
    }

    /* the kernel image: the top 2 GB show the first 2 GB of physical memory (the same tables) */
    uint64_t *pdpt_kernel = new_table();
    pdpt_kernel[510] = V2P(first_pds[0]) | PTE_PRESENT | PTE_WRITABLE;
    pdpt_kernel[511] = V2P(first_pds[1]) | PTE_PRESENT | PTE_WRITABLE;
    kernel_pml4[511] = V2P(pdpt_kernel) | PTE_PRESENT | PTE_WRITABLE;

    paging_switch(kernel_pml4);
    terminal_writestring("[OK] Paging enabled\n");
}

void paging_map(uint64_t virt, uint64_t phys, uint32_t flags)
{
    (void)virt; (void)phys; (void)flags;
}

void paging_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags)
{
    (void)virt; (void)phys; (void)size; (void)flags;
}

void paging_switch(uint64_t *dir)
{
    asm volatile("mov %0, %%cr3" : : "r"(V2P(dir)) : "memory");
}

/* A new address space: the kernel half is the kernel's own (shared), the user half starts empty. */
uint64_t *paging_create_dir(void)
{
    uint64_t *dir = new_table();
    if (!dir) return 0;
    for (int i = 256; i < 512; i++) dir[i] = kernel_pml4[i];
    return dir;
}

static void free_level(uint64_t *t, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_PRESENT) && !(level == 1 && (t[i] & PTE_SAVED))) continue;
        if (level == 1 || (t[i] & PTE_HUGE)) {
            if (level == 1) free_physical_page((uint32_t)(t[i] & ADDR_MASK));
            continue;
        }
        free_level(table(t[i]), level - 1);
        free_physical_page((uint32_t)(t[i] & ADDR_MASK));
    }
}

/* Frees tables made by paging_create_dir() and every page in their user half. */
void paging_destroy_dir(uint64_t *dir)
{
    if (!dir || dir == kernel_pml4 || paging_current_dir() == dir) return;
    for (int i = 0; i < 256; i++) {
        if (!(dir[i] & PTE_PRESENT)) continue;
        free_level(table(dir[i]), 3);
        free_physical_page((uint32_t)(dir[i] & ADDR_MASK));
    }
    free_physical_page((uint32_t)V2P(dir));
}

/* the page-table entry for a user address, made (with the tables above it) when `make` */
static uint64_t *pte_of(uint64_t *dir, uint64_t virt, int make)
{
    if (virt >= PAGING_USER_END) return 0;
    uint64_t *t = dir;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &t[(virt >> shift) & 511];
        if (!(*e & PTE_PRESENT)) {
            if (!make) return 0;
            uint64_t *n = new_table();
            if (!n) return 0;
            *e = V2P(n) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        t = table(*e);
    }
    return &t[(virt >> 12) & 511];
}

static void flush(uint64_t *dir, uint64_t virt)
{
    if (dir == paging_current_dir()) asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

int paging_map_in(uint64_t *dir, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pte = pte_of(dir, virt, 1);
    if (!pte) return -1;
    *pte = (phys & ADDR_MASK) | PTE_PRESENT | (flags & (0xFFFull | PTE_NX));
    flush(dir, virt);
    return 0;
}

int paging_unmap_in(uint64_t *dir, uint64_t virt, uint64_t *phys_out)
{
    if (!dir || (virt & (PAGE_SIZE - 1u)) || virt < PAGING_USER_START || virt >= PAGING_USER_END) return -1;
    uint64_t *pte = pte_of(dir, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    if (phys_out) *phys_out = *pte & ADDR_MASK;
    *pte = 0;
    flush(dir, virt);
    return 1;
}

uint64_t paging_cow_flags(uint64_t phys, uint64_t flags)
{
    if ((flags & PTE_WRITABLE) && page_is_shared((uint32_t)phys))
        flags = (flags & ~(uint64_t)PTE_WRITABLE) | PTE_COW;
    return flags;
}

int paging_protect_in(uint64_t *dir, uint64_t virt, uint64_t flags)
{
    if (!dir || (virt & (PAGE_SIZE - 1u)) || virt < PAGING_USER_START || virt >= PAGING_USER_END) return -1;
    uint64_t *pte = pte_of(dir, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    flags = paging_cow_flags(*pte & ADDR_MASK, flags & ~(uint64_t)PTE_COW);
    *pte = (*pte & ADDR_MASK) | PTE_PRESENT | (flags & (0xFFFull | PTE_NX));
    flush(dir, virt);
    return 0;
}

int paging_user_range_ok(uint64_t virt, uint64_t len, int write)
{
    if (len == 0) return 0;
    uint64_t end = virt + len;
    if (end < virt || virt < PAGING_USER_START || end > PAGING_USER_END) return 0;
    uint64_t *dir = paging_current_dir();
    for (uint64_t page = virt & ~0xFFFull; page < end; page += PAGE_SIZE) {
        uint64_t *t = dir;
        for (int shift = 39; shift > 12; shift -= 9) {
            uint64_t e = t[(page >> shift) & 511];
            if (!(e & PTE_PRESENT) || !(e & PTE_USER)) return 0;
            t = table(e);
        }
        uint64_t pte = t[(page >> 12) & 511];
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return 0;
        if (write && !(pte & PTE_WRITABLE) && (!(pte & PTE_COW) || paging_cow_break(dir, page) != 1)) return 0;
    }
    return 1;
}

uint64_t paging_virt_to_phys(uint64_t *dir, uint64_t virt)
{
    if (!dir) dir = paging_current_dir();
    if (virt >= KERNEL_VMA) return virt - KERNEL_VMA;
    if (virt >= DIRECT_MAP_BASE) return virt - DIRECT_MAP_BASE;
    uint64_t *pte = pte_of(dir, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & ADDR_MASK) | (virt & 0xFFF);
}

int paging_saved_page(uint64_t *dir, uint64_t virt)
{
    uint64_t *pte = pte_of(dir, virt, 0);
    return pte && !(*pte & PTE_PRESENT) && (*pte & PTE_SAVED);
}

static int fork_level(uint64_t *src, uint64_t *dst, int level)
{
    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (level == 1) {
            if (!(e & (PTE_PRESENT | PTE_SAVED))) continue;
            if (page_ref_inc((uint32_t)(e & ADDR_MASK)) != 0) return -1;
            if (e & PTE_WRITABLE) {
                e = (e & ~(uint64_t)PTE_WRITABLE) | PTE_COW;
                src[i] = e;
            }
            dst[i] = e;
            continue;
        }
        if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) continue;
        uint64_t *n = new_table();
        if (!n) return -1;
        dst[i] = V2P(n) | (e & ~ADDR_MASK);
        if (fork_level(table(e), n, level - 1) != 0) return -1;
    }
    return 0;
}

uint64_t *paging_fork_dir(uint64_t *src)
{
    uint64_t *dir = paging_create_dir();
    if (!dir) return 0;
    int rc = 0;
    for (int i = 0; i < 256 && !rc; i++) {
        if (!(src[i] & PTE_PRESENT)) continue;
        uint64_t *n = new_table();
        if (!n) { rc = -1; break; }
        dir[i] = V2P(n) | (src[i] & ~ADDR_MASK);
        rc = fork_level(table(src[i]), n, 3);
    }
    if (src == paging_current_dir()) paging_switch(src);     /* the parent's pages are read-only now */
    if (rc) { paging_destroy_dir(dir); return 0; }
    return dir;
}

int paging_cow_break(uint64_t *dir, uint64_t virt)
{
    uint64_t *pte = pte_of(dir, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW)) return 0;
    uint32_t phys = (uint32_t)(*pte & ADDR_MASK);
    uint64_t flags = (*pte & ~ADDR_MASK & ~(uint64_t)PTE_COW) | PTE_WRITABLE;
    if (page_is_shared(phys)) {
        uint32_t copy = alloc_physical_page();
        if (!copy) return -1;
        memcpy(P2V(copy), P2V(phys), PAGE_SIZE);
        *pte = copy | flags;
        free_physical_page(phys);                         /* one owner fewer */
    } else {
        *pte = phys | flags;
    }
    flush(dir, virt & ~0xFFFull);
    return 1;
}
