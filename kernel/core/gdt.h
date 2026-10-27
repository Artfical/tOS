#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_tss(uint32_t tss_addr);
int gdt_tls_alloc(int requested_entry);
void gdt_tls_release(int entry);
int gdt_tls_set(int entry, uint32_t base, uint32_t limit,
                uint8_t access, uint8_t granularity);

#endif
