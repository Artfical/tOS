#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER32_CS 0x1B
#define GDT_USER_DS   0x23
#define GDT_USER64_CS 0x2B
#define GDT_KERNEL_CS32 0xC0

void gdt_init(void);
void gdt_set_tss(uint64_t tss_addr, uint32_t size);
int gdt_tls_alloc(int requested_entry);
void gdt_tls_release(int entry);
int gdt_tls_set(int entry, uint32_t base, uint32_t limit,
                uint8_t access, uint8_t granularity);

#endif
