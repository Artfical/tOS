#include "msr.h"
void msr_read(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    asm volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}
void msr_write(uint32_t msr, uint32_t lo, uint32_t hi)
{
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}
uint64_t msr_read64(uint32_t msr)
{
    uint32_t lo, hi;
    msr_read(msr, &lo, &hi);
    return ((uint64_t)hi << 32) | lo;
}
void msr_write64(uint32_t msr, uint64_t val)
{
    msr_write(msr, val & 0xFFFFFFFF, val >> 32);
}
