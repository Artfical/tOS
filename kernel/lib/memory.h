#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define KERNEL_HEAP_START 0x1000000
#define KERNEL_HEAP_INITIAL_SIZE 0x100000
/* Was 0x400000 (4MB total) -- far too small for a single large
 * allocation like DOOM's zone allocator (Z_Init wants a 6-16MB
 * contiguous block, see kernel/doom/i_system.c's I_ZoneBase()), which
 * failed outright and made DOOM call exit() during startup. Raised to
 * 64MB, well within the ~254MB paging_init() already identity-maps at
 * boot on a typical QEMU/VMware VM size, with plenty of margin left
 * over for everything else the kernel allocates. */
#define KERNEL_HEAP_MAX_SIZE 0x4000000

/* reserved_end: end of any extra memory range (e.g. a multiboot
 * module like the initrd) that must not be handed out by the heap.
 * Without this, heap_start was computed purely from the kernel
 * binary's own end -- fine as long as nothing else lived past that,
 * but the initrd module is loaded by GRUB into its own separate
 * region which can sit right after the kernel too. A large enough
 * allocation (first hit by DOOM's ~4MB WAD, nothing before it ever
 * grew the heap this far) could then grow straight into and past the
 * initrd's own memory -- corrupting the initrd's remaining
 * not-yet-imported data while ramfs_import_initrd() was still reading
 * from that same physical memory. Pass 0 if there's no such region. */
void memory_init(uint32_t mem_upper, uint32_t reserved_end);
void *malloc(size_t size);
void free(void *ptr);
void *krealloc(void *ptr, size_t size);
void *kcalloc(size_t num, size_t size);
void memory_get_usage(uint32_t *total_kb, uint32_t *used_kb);

uint32_t alloc_physical_page(void);
void free_physical_page(uint32_t addr);
uint32_t get_total_pages(void);
uint32_t get_used_pages(void);

/* Debug-only: walks the free list checking every header's magic
 * number, printing a marker via terminal_writestring if it finds one
 * that's corrupted. Prints tag so callers can bracket where in their
 * own code the corruption first appears. */
void heap_check(const char *tag);

#endif
