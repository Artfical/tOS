#include "idt.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt_entries[256];
static struct idt_ptr idt_ptr;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags)
{
    idt_entries[num].base_low = base & 0xFFFF;
    idt_entries[num].base_mid = (base >> 16) & 0xFFFF;
    idt_entries[num].base_high = (uint32_t)(base >> 32);
    idt_entries[num].sel = sel;
    idt_entries[num].ist = 0;
    idt_entries[num].flags = flags;
    idt_entries[num].reserved = 0;
}

void idt_init(void)
{
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base = (uint64_t)&idt_entries;
    asm volatile("lidt %0" : : "m"(idt_ptr));
}
