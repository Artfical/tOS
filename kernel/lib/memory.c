#include "memory.h"
#include "terminal.h"
#include "string.h"

static uint32_t *page_bitmap = NULL;
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

static uint32_t heap_start = KERNEL_HEAP_START;
static uint32_t heap_end = KERNEL_HEAP_START + KERNEL_HEAP_INITIAL_SIZE;
static uint32_t heap_current = KERNEL_HEAP_START;

/* size is the payload capacity handed to the caller; a 4-byte canary
 * word immediately follows the payload (before the next block's own
 * header), so a caller that writes even one byte past what it asked
 * for corrupts a value the allocator itself checks on free() instead
 * of silently clobbering the next block's header -- which is what
 * used to happen, and free-list walks would eventually dereference
 * whatever garbage that overwrite left in a `next` pointer and
 * page-fault deep inside heap_alloc(), far from the actual bug. */
typedef struct heap_header {
    uint32_t magic;
    uint32_t size;
    uint32_t req_size;
    uint32_t used;
    struct heap_header *next;
} heap_header_t;

#define HEAP_MAGIC   0xDEADBEEF
#define HEAP_CANARY  0xC0FFEEEE

/* Total bytes a block with `size`-byte payload occupies, header
 * through canary, i.e. the distance from this block's header to the
 * next one -- every place that used to compute a "next block" address
 * as sizeof(heap_header_t) + size now goes through this instead, so
 * the canary reservation can't be forgotten in just one of the spots. */
#define HEAP_SLOT(size) (sizeof(heap_header_t) + (size) + 4)

static heap_header_t *heap_list = NULL;
static int heap_corrupt_reported = 0;

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
    heap_list->req_size = 0;
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

static void heap_report(const char *msg)
{
    terminal_writestring("[heap] ");
    terminal_writestring(msg);
    terminal_writestring("\n");
}

/* A header pointer is only trustworthy if it falls inside the heap's
 * current address range and is properly aligned -- every raw pointer
 * this file follows (a `next` link, or the header implied by a
 * pointer passed to free()) goes through this before being
 * dereferenced, so a corrupted link becomes a reported, contained
 * failure instead of an unchecked read that can fault anywhere. */
static int heap_ptr_ok(heap_header_t *h)
{
    uint32_t addr = (uint32_t)h;
    if (addr < heap_start || addr >= heap_end) return 0;
    if (addr % 4 != 0) return 0;
    return 1;
}

static void *heap_alloc(uint32_t size)
{
    if (size == 0) return NULL;
    size = (size + 3) & ~3;

    uint32_t flags = heap_irq_save();

    heap_header_t *curr = heap_list;
    while (curr) {
        if (!heap_ptr_ok(curr) || curr->magic != HEAP_MAGIC) {
            if (!heap_corrupt_reported) {
                heap_report("corrupted free-list node found during malloc, list walk aborted");
                heap_corrupt_reported = 1;
            }
            break;
        }
        if (!curr->used && curr->size >= size + 4) {
            if (curr->size > size + HEAP_SLOT(0) + 16) {
                heap_header_t *new_hdr = (heap_header_t *)((uint32_t)curr + HEAP_SLOT(size));
                new_hdr->magic = HEAP_MAGIC;
                new_hdr->size = curr->size - size - HEAP_SLOT(0);
                new_hdr->req_size = 0;
                new_hdr->used = 0;
                new_hdr->next = curr->next;
                curr->size = size + 4;
                curr->next = new_hdr;
            }
            curr->req_size = size;
            curr->used = 1;
            *(uint32_t *)((uint8_t *)curr + sizeof(heap_header_t) + curr->size - 4) = HEAP_CANARY;
            heap_irq_restore(flags);
            return (void *)((uint32_t)curr + sizeof(heap_header_t));
        }
        curr = curr->next;
    }

    uint32_t old_heap_end = heap_end;
    heap_end += 0x100000;
    if (heap_end > KERNEL_HEAP_START + 0x400000)
        heap_end = KERNEL_HEAP_START + 0x400000;

    if (old_heap_end + HEAP_SLOT(size) > heap_end) {
        heap_irq_restore(flags);
        return NULL;
    }

    heap_header_t *new_hdr = (heap_header_t *)old_heap_end;
    new_hdr->magic = HEAP_MAGIC;
    new_hdr->size = heap_end - old_heap_end - sizeof(heap_header_t);
    new_hdr->req_size = 0;
    new_hdr->used = 0;
    new_hdr->next = NULL;

    heap_header_t *last = heap_list;
    int hops = 0;
    while (last->next) {
        if (!heap_ptr_ok(last->next) || last->next->magic != HEAP_MAGIC) {
            if (!heap_corrupt_reported) {
                heap_report("corrupted free-list node found while appending new heap region");
                heap_corrupt_reported = 1;
            }
            last->next = NULL;
            break;
        }
        last = last->next;
        if (++hops > 1000000) { heap_irq_restore(flags); return NULL; }
    }
    last->next = new_hdr;

    if (new_hdr->size >= size + 4) {
        if (new_hdr->size > size + HEAP_SLOT(0) + 16) {
            heap_header_t *split = (heap_header_t *)((uint32_t)new_hdr + HEAP_SLOT(size));
            split->magic = HEAP_MAGIC;
            split->size = new_hdr->size - size - HEAP_SLOT(0);
            split->req_size = 0;
            split->used = 0;
            split->next = NULL;
            new_hdr->size = size + 4;
            new_hdr->next = split;
        }
        new_hdr->req_size = size;
        new_hdr->used = 1;
        *(uint32_t *)((uint8_t *)new_hdr + sizeof(heap_header_t) + new_hdr->size - 4) = HEAP_CANARY;
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

void heap_check(const char *tag)
{
    terminal_writestring("[heap_check] ");
    terminal_writestring(tag);
    heap_header_t *curr = heap_list;
    int i = 0;
    while (curr) {
        if (!heap_ptr_ok(curr) || curr->magic != HEAP_MAGIC) {
            terminal_writestring(" CORRUPT at node ");
            char buf[12]; int n = 0;
            uint32_t v = (uint32_t)i;
            if (v == 0) buf[n++] = '0';
            while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
            for (int j = 0; j < n / 2; j++) { char t = buf[j]; buf[j] = buf[n-1-j]; buf[n-1-j] = t; }
            buf[n] = 0;
            terminal_writestring(buf);
            terminal_writestring("\n");
            return;
        }
        if (curr->used) {
            uint32_t canary = *(uint32_t *)((uint8_t *)curr + sizeof(heap_header_t) + curr->size - 4);
            if (canary != HEAP_CANARY) {
                char nb[12]; int nn = 0;
                terminal_writestring(" OVERFLOW at node ");
                uint32_t v = (uint32_t)i;
                if (v == 0) nb[nn++] = '0';
                while (v > 0) { nb[nn++] = '0' + (v % 10); v /= 10; }
                for (int j = 0; j < nn / 2; j++) { char t = nb[j]; nb[j] = nb[nn-1-j]; nb[nn-1-j] = t; }
                nb[nn] = 0;
                terminal_writestring(nb);
                terminal_writestring(" req_size=");
                nn = 0;
                v = curr->req_size;
                if (v == 0) nb[nn++] = '0';
                while (v > 0) { nb[nn++] = '0' + (v % 10); v /= 10; }
                for (int j = 0; j < nn / 2; j++) { char t = nb[j]; nb[j] = nb[nn-1-j]; nb[nn-1-j] = t; }
                nb[nn] = 0;
                terminal_writestring(nb);
                terminal_writestring(" addr=");
                char ab[9];
                uint32_t addr = (uint32_t)curr;
                for (int k = 7; k >= 0; k--) { ab[k] = "0123456789ABCDEF"[addr & 0xF]; addr >>= 4; }
                ab[8] = 0;
                terminal_writestring(ab);
                uint32_t canary_bytes = canary;
                terminal_writestring(" canary_read=");
                for (int k = 7; k >= 0; k--) { ab[k] = "0123456789ABCDEF"[canary_bytes & 0xF]; canary_bytes >>= 4; }
                ab[8] = 0;
                terminal_writestring(ab);
                terminal_writestring("\n");
                return;
            }
        }
        curr = curr->next;
        i++;
        if (i > 100000) { terminal_writestring(" (giving up after 100000 nodes)\n"); return; }
    }
    terminal_writestring(" OK\n");
}

void free(void *ptr)
{
    if (!ptr) return;
    heap_header_t *hdr = (heap_header_t *)((uint32_t)ptr - sizeof(heap_header_t));

    uint32_t flags = heap_irq_save();

    if (!heap_ptr_ok(hdr) || hdr->magic != HEAP_MAGIC) {
        heap_irq_restore(flags);
        heap_report("free() called with an invalid/non-heap pointer, ignored");
        return;
    }
    if (!hdr->used) {
        heap_irq_restore(flags);
        heap_report("double free() detected, ignored");
        return;
    }

    uint32_t canary = *(uint32_t *)((uint8_t *)hdr + sizeof(heap_header_t) + hdr->size - 4);
    int overflowed = (canary != HEAP_CANARY);

    /* Still marks it free either way -- refusing to free would leak
     * forever, and the offending write already happened; the goal
     * here is a loud diagnostic instead of a silent, much-later crash
     * somewhere unrelated when a future malloc() walks into whatever
     * this overflow clobbered. */
    hdr->used = 0;
    heap_irq_restore(flags);

    if (overflowed) heap_report("buffer overflow detected: block wrote past its own end");
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    heap_header_t *hdr = (heap_header_t *)((uint32_t)ptr - sizeof(heap_header_t));
    if (!heap_ptr_ok(hdr) || hdr->magic != HEAP_MAGIC) return NULL;
    if (hdr->size >= size + 4) return ptr;
    void *new_ptr = malloc(size);
    if (new_ptr) {
        uint32_t old_data = hdr->req_size;
        memcpy(new_ptr, ptr, old_data < size ? old_data : size);
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
