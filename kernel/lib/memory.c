#include "memory.h"
#include "terminal.h"
#include "string.h"

static uint32_t *page_bitmap = NULL;
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

static uint32_t heap_start = KERNEL_HEAP_START;
static uint32_t heap_end = KERNEL_HEAP_START + KERNEL_HEAP_INITIAL_SIZE;
static uint32_t heap_current = KERNEL_HEAP_START;

typedef struct heap_header {
    uint32_t magic;
    uint32_t size;
    uint32_t used;
    struct heap_header *next;
} heap_header_t;

static heap_header_t *heap_list = NULL;
#define HEAP_MAGIC 0xDEADBEEF

void memory_init(uint32_t mem_upper)
{
    total_pages = (mem_upper * 1024) / PAGE_SIZE;
    if (total_pages > 0x100000) total_pages = 0x100000;

    uint32_t bitmap_size = total_pages / 8 + 1;
    page_bitmap = (uint32_t *)(KERNEL_HEAP_START);
    memset(page_bitmap, 0, bitmap_size);

    uint32_t kernel_end = 0;
    extern uint32_t end;
    kernel_end = (uint32_t)&end;

    for (uint32_t i = 0; i < kernel_end / PAGE_SIZE + 1; i++) {
        page_bitmap[i / 32] |= (1 << (i % 32));
        used_pages++;
    }

    uint32_t heap_align = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (heap_align > KERNEL_HEAP_START)
        heap_align = KERNEL_HEAP_START;
    heap_start = heap_align;
    heap_current = heap_align;
    heap_end = heap_align + KERNEL_HEAP_INITIAL_SIZE;

    heap_list = (heap_header_t *)heap_start;
    heap_list->magic = HEAP_MAGIC;
    heap_list->size = KERNEL_HEAP_INITIAL_SIZE - sizeof(heap_header_t);
    heap_list->used = 0;
    heap_list->next = NULL;

    uint32_t bitmap_end = ((uint32_t)page_bitmap + bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t i = kernel_end / PAGE_SIZE + 1; i < bitmap_end / PAGE_SIZE; i++) {
        page_bitmap[i / 32] |= (1 << (i % 32));
        used_pages++;
    }

    uint32_t heap_end_page = (KERNEL_HEAP_START + KERNEL_HEAP_INITIAL_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = bitmap_end / PAGE_SIZE; i < heap_end_page; i++) {
        page_bitmap[i / 32] |= (1 << (i % 32));
        used_pages++;
    }

    terminal_writestring("Memory init: ");
    char buf[32];
    int i = 0;
    uint32_t n = total_pages * 4;
    if (n >= 1024) {
        n /= 1024;
        while (n) { buf[i++] = '0' + n % 10; n /= 10; }
        for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
        buf[i] = '\0';
        terminal_writestring(buf);
        terminal_writestring(" MB\n");
    }
}

uint32_t alloc_physical_page(void)
{
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!(page_bitmap[i / 32] & (1 << (i % 32)))) {
            page_bitmap[i / 32] |= (1 << (i % 32));
            used_pages++;
            uint32_t addr = i * PAGE_SIZE;
            memset((void *)addr, 0, PAGE_SIZE);
            return addr;
        }
    }
    return 0;
}

void free_physical_page(uint32_t addr)
{
    uint32_t page = addr / PAGE_SIZE;
    if (page >= total_pages) return;
    page_bitmap[page / 32] &= ~(1 << (page % 32));
    used_pages--;
}

uint32_t get_total_pages(void) { return total_pages; }
uint32_t get_used_pages(void)  { return used_pages; }

static inline uint32_t heap_irq_save(void)
{
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
    return flags;
}

static inline void heap_irq_restore(uint32_t flags)
{
    asm volatile("pushl %0; popfl" : : "r"(flags));
}

static void *heap_alloc(uint32_t size)
{
    if (size == 0) return NULL;
    size = (size + 3) & ~3;

    uint32_t flags = heap_irq_save();

    heap_header_t *curr = heap_list;
    while (curr) {
        if (!curr->used && curr->size >= size) {
            if (curr->size > size + sizeof(heap_header_t) + 16) {
                heap_header_t *new_hdr = (heap_header_t *)((uint32_t)curr + sizeof(heap_header_t) + size);
                new_hdr->magic = HEAP_MAGIC;
                new_hdr->size = curr->size - size - sizeof(heap_header_t);
                new_hdr->used = 0;
                new_hdr->next = curr->next;
                curr->size = size;
                curr->next = new_hdr;
            }
            curr->used = 1;
            heap_irq_restore(flags);
            return (void *)((uint32_t)curr + sizeof(heap_header_t));
        }
        curr = curr->next;
    }

    uint32_t old_heap_end = heap_end;
    heap_end += 0x100000;
    if (heap_end > KERNEL_HEAP_START + 0x400000)
        heap_end = KERNEL_HEAP_START + 0x400000;

    heap_header_t *new_hdr = (heap_header_t *)old_heap_end;
    new_hdr->magic = HEAP_MAGIC;
    new_hdr->size = heap_end - old_heap_end - sizeof(heap_header_t);
    new_hdr->used = 0;
    new_hdr->next = NULL;

    heap_header_t *last = heap_list;
    while (last->next) last = last->next;
    last->next = new_hdr;

    if (new_hdr->size >= size) {
        if (new_hdr->size > size + sizeof(heap_header_t) + 16) {
            heap_header_t *split = (heap_header_t *)((uint32_t)new_hdr + sizeof(heap_header_t) + size);
            split->magic = HEAP_MAGIC;
            split->size = new_hdr->size - size - sizeof(heap_header_t);
            split->used = 0;
            split->next = NULL;
            new_hdr->size = size;
            new_hdr->next = split;
        }
        new_hdr->used = 1;
        heap_irq_restore(flags);
        return (void *)((uint32_t)new_hdr + sizeof(heap_header_t));
    }

    heap_irq_restore(flags);
    return NULL;
}

void *malloc(size_t size)
{
    return heap_alloc((uint32_t)size);
}

void free(void *ptr)
{
    if (!ptr) return;
    heap_header_t *hdr = (heap_header_t *)((uint32_t)ptr - sizeof(heap_header_t));
    uint32_t flags = heap_irq_save();
    if (hdr->magic != HEAP_MAGIC) { heap_irq_restore(flags); return; }
    hdr->used = 0;
    heap_irq_restore(flags);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    heap_header_t *hdr = (heap_header_t *)((uint32_t)ptr - sizeof(heap_header_t));
    if (hdr->magic != HEAP_MAGIC) return NULL;
    if (hdr->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, hdr->size < size ? hdr->size : size);
        free(ptr);
    }
    return new_ptr;
}

void *kcalloc(size_t num, size_t size)
{
    void *ptr = malloc(num * size);
    if (ptr) memset(ptr, 0, num * size);
    return ptr;
}

void memory_get_usage(uint32_t *total_kb, uint32_t *used_kb)
{
    *total_kb = total_pages * 4;
    uint32_t kernel_end = 0;
    extern uint32_t end;
    kernel_end = (uint32_t)&end;
    *used_kb = (kernel_end / 1024) + (heap_current - heap_start) / 1024;
    if (*used_kb > *total_kb) *used_kb = *total_kb;
}
