#include "paging.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"

static uint32_t *kernel_dir = NULL;

void paging_init(void)
{
    kernel_dir = (uint32_t *)alloc_physical_page();
    memset(kernel_dir, 0, PAGE_SIZE);

    for (uint32_t virt = 0; virt < 0x1000000; virt += 0x400000) {
        uint32_t pd_idx = virt >> 22;
        uint32_t *pt = (uint32_t *)alloc_physical_page();
        memset(pt, 0, PAGE_SIZE);

        uint32_t pt_flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t phys = (virt + j * PAGE_SIZE);
            pt[j] = phys | pt_flags;
        }

        kernel_dir[pd_idx] = ((uint32_t)pt) | PDE_PRESENT | PDE_WRITABLE | PDE_USER;
    }

    uint32_t vga_page = 0xB8000 >> 22;
    uint32_t vga_table_idx = (0xB8000 >> 12) & 0x3FF;
    uint32_t *vga_pt = (uint32_t *)(kernel_dir[vga_page] & 0xFFFFF000);
    if (vga_pt) {
        vga_pt[vga_table_idx] = 0xB8000 | PTE_PRESENT | PTE_WRITABLE;
    }

    paging_switch(kernel_dir);

    terminal_writestring("[OK] Paging enabled\n");
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(kernel_dir[pd_idx] & PDE_PRESENT)) {
        uint32_t *pt = (uint32_t *)alloc_physical_page();
        memset(pt, 0, PAGE_SIZE);
        kernel_dir[pd_idx] = ((uint32_t)pt) | PDE_PRESENT | PDE_WRITABLE | (flags & PDE_USER);
    }

    uint32_t *pt = (uint32_t *)(kernel_dir[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = (phys & 0xFFFFF000) | PTE_PRESENT | (flags & 0xFFF);
}

void paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags)
{
    for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE)
        paging_map(virt + offset, phys + offset, flags);
}

void paging_switch(uint32_t *dir)
{
    asm volatile("mov %0, %%cr3" : : "r"(dir));
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
}

uint32_t *paging_create_dir(void)
{
    uint32_t *dir = (uint32_t *)alloc_physical_page();
    memset(dir, 0, PAGE_SIZE);

    for (int i = 0; i < 1024 && kernel_dir; i++) {
        if (kernel_dir[i] & PDE_PRESENT) {
            uint32_t phys = kernel_dir[i] & 0xFFFFF000;
            uint32_t flags = kernel_dir[i] & 0xFFF;
            dir[i] = phys | flags;
        }
    }

    return dir;
}

void paging_destroy_dir(uint32_t *dir)
{
    if (!dir || dir == kernel_dir) return;
    for (int i = 0; i < 1024; i++) {
        if (dir[i] & PDE_PRESENT) {
            uint32_t pt_phys = dir[i] & 0xFFFFF000;
            free_physical_page(pt_phys);
        }
    }
    free_physical_page((uint32_t)dir);
}

uint32_t paging_virt_to_phys(uint32_t *dir, uint32_t virt)
{
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pd_phys;
    if (dir) {
        pd_phys = (uint32_t)dir;
    } else {
        asm volatile("mov %%cr3, %0" : "=r"(pd_phys));
    }

    uint32_t *pd = (uint32_t *)pd_phys;
    if (!(pd[pd_idx] & PDE_PRESENT))
        return 0;

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & 0xFFFFF000);
    if (!(pt[pt_idx] & PTE_PRESENT))
        return 0;

    return (pt[pt_idx] & 0xFFFFF000) | (virt & 0xFFF);
}
