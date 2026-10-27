#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/* the 64-bit TSS: only rsp0 (the stack an interrupt from ring 3 starts on) and the I/O map matter */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap;
} __attribute__((packed)) tss_t;

void tss_init(void);
void tss_set_kernel_stack(uint64_t rsp);

#endif
