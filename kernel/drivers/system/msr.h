#ifndef MSR_H
#define MSR_H
#include <stdint.h>
#define MSR_APIC_BASE 0x1B
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_CSTAR 0xC0000083
#define MSR_SFMASK 0xC0000084
#define MSR_MTRR_PHYS_BASE0 0x200
#define MSR_MTRR_PHYS_MASK0 0x201
#define MSR_MTRR_DEF_TYPE 0x2FF
#define MSR_PAT 0x277
void msr_write(uint32_t msr, uint32_t lo, uint32_t hi);
void msr_read(uint32_t msr, uint32_t *lo, uint32_t *hi);
uint64_t msr_read64(uint32_t msr);
void msr_write64(uint32_t msr, uint64_t val);
#endif
