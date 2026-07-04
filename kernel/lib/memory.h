#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define KERNEL_HEAP_START 0x1000000
#define KERNEL_HEAP_INITIAL_SIZE 0x100000

void memory_init(uint32_t mem_upper);
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
