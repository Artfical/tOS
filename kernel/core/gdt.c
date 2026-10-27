#include "gdt.h"

#define GDT_TLS_FIRST 6
#define GDT_TLS_SLOTS 16

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
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt_entries[GDT_TLS_FIRST + GDT_TLS_SLOTS];
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

void gdt_set_tss(uint32_t tss_addr)
{
    uint32_t base = tss_addr;
    uint32_t limit = 104 - 1; /* sizeof(tss_t) - 1 */
    gdt_set_gate(5, base, limit, 0x89, 0x40);
    asm volatile("ltr %%ax" : : "a"(5 << 3));
}

int gdt_tls_alloc(int requested_entry)
{
    uint32_t flags;
    int result = -1;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
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
    asm volatile("pushl %0; popfl" : : "r"(flags));
    return result;
}

void gdt_tls_release(int entry)
{
    if (entry < GDT_TLS_FIRST || entry >= GDT_TLS_FIRST + GDT_TLS_SLOTS) return;
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
    gdt_tls_used &= ~(1u << (entry - GDT_TLS_FIRST));
    gdt_set_gate(entry, 0, 0, 0, 0);
    asm volatile("pushl %0; popfl" : : "r"(flags));
}

int gdt_tls_set(int entry, uint32_t base, uint32_t limit,
                uint8_t access, uint8_t granularity)
{
    if (entry < GDT_TLS_FIRST || entry >= GDT_TLS_FIRST + GDT_TLS_SLOTS ||
        !(gdt_tls_used & (1u << (entry - GDT_TLS_FIRST)))) return -1;
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
    gdt_set_gate(entry, base, limit, access, granularity);
    asm volatile("pushl %0; popfl" : : "r"(flags));
    return 0;
}

void gdt_init(void)
{
    gdt_tls_used = 0;
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate(5, 0, 0, 0x89, 0);

    asm volatile("lgdt %0" : : "m"(gdt_ptr));
    asm volatile(
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%fs\n"
        "mov %0, %%gs\n"
        "mov %0, %%ss\n"
        "ljmp %1, $1f\n"
        "1:\n"
        :
        : "r"(0x10), "i"(0x08)
        : "memory"
    );
}
