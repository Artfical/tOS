#include "gdt.h"

/* 0x08 kernel code (64-bit), 0x10 kernel data, 0x1B user code (32-bit programs, compatibility
 * mode), 0x23 user data, 0x2B user code (64-bit programs) -- the order syscall/sysret expect --
 * then the TSS (two entries in long mode) and the TLS entries 32-bit programs ask for. */
#define GDT_TSS        6
#define GDT_TLS_FIRST  8
#define GDT_TLS_SLOTS 16
#define GDT_KCODE32   (GDT_TLS_FIRST + GDT_TLS_SLOTS)    /* 0xC0: kexec leaves long mode through it */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt_entries[GDT_KCODE32 + 1];
static struct gdt_ptr gdt_ptr;
static uint32_t gdt_tls_used;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low = base & 0xFFFF;
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high = (base >> 24) & 0xFF;
    gdt_entries[num].limit_low = limit & 0xFFFF;
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;
    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access = access;
}

static uint64_t irq_save(void)
{
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags)
{
    asm volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

void gdt_set_tss(uint64_t tss_addr, uint32_t size)
{
    gdt_set_gate(GDT_TSS, (uint32_t)tss_addr, size - 1, 0x89, 0x00);
    /* the upper half of the 64-bit base, in the next entry */
    uint32_t *hi = (uint32_t *)&gdt_entries[GDT_TSS + 1];
    hi[0] = (uint32_t)(tss_addr >> 32);
    hi[1] = 0;
    asm volatile("ltr %%ax" : : "a"(GDT_TSS << 3));
}

int gdt_tls_alloc(int requested_entry)
{
    int result = -1;
    uint64_t flags = irq_save();
    if (requested_entry >= GDT_TLS_FIRST &&
        requested_entry < GDT_TLS_FIRST + GDT_TLS_SLOTS) {
        uint32_t bit = 1u << (requested_entry - GDT_TLS_FIRST);
        if (!(gdt_tls_used & bit)) {
            gdt_tls_used |= bit;
            result = requested_entry;
        }
    } else if (requested_entry < 0) {
        for (int i = 0; i < GDT_TLS_SLOTS; i++) {
            uint32_t bit = 1u << i;
            if (gdt_tls_used & bit) continue;
            gdt_tls_used |= bit;
            result = GDT_TLS_FIRST + i;
            break;
        }
    }
    irq_restore(flags);
    return result;
}

void gdt_tls_release(int entry)
{
    if (entry < GDT_TLS_FIRST || entry >= GDT_TLS_FIRST + GDT_TLS_SLOTS) return;
    uint64_t flags = irq_save();
    gdt_tls_used &= ~(1u << (entry - GDT_TLS_FIRST));
    gdt_set_gate(entry, 0, 0, 0, 0);
    irq_restore(flags);
}

int gdt_tls_set(int entry, uint32_t base, uint32_t limit,
                uint8_t access, uint8_t granularity)
{
    if (entry < GDT_TLS_FIRST || entry >= GDT_TLS_FIRST + GDT_TLS_SLOTS ||
        !(gdt_tls_used & (1u << (entry - GDT_TLS_FIRST)))) return -1;
    uint64_t flags = irq_save();
    gdt_set_gate(entry, base, limit, access, granularity);
    irq_restore(flags);
    return 0;
}

void gdt_init(void)
{
    gdt_tls_used = 0;
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base = (uint64_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);     /* kernel code, 64-bit (L) */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xC0);     /* kernel data */
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xC0);     /* user code, 32-bit */
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0);     /* user data */
    gdt_set_gate(5, 0, 0xFFFFF, 0xFA, 0xA0);     /* user code, 64-bit */
    gdt_set_gate(GDT_TSS, 0, 0, 0, 0);
    gdt_set_gate(GDT_TSS + 1, 0, 0, 0, 0);
    gdt_set_gate(GDT_KCODE32, 0, 0xFFFFF, 0x9A, 0xC0);   /* kernel code, 32-bit */

    asm volatile("lgdt %0" : : "m"(gdt_ptr));
    asm volatile(
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "r"(0x10)
        : "rax", "memory"
    );
}
