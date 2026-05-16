#include "fpu.h"
int fpu_init(fpu_device_t *fpu)
{
    fpu->present = 0;
    fpu->has_sse = 0;
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(FPU_CR0_EM | FPU_CR0_TS);
    cr0 |= FPU_CR0_MP | FPU_CR0_NE;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
    asm volatile("fninit");
    uint16_t fsw = 0;
    asm volatile("fnstsw %0" : "=m"(fsw));
    if ((fsw & 0xFF) == 0) fpu->present = 1;
    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & FPU_CR4_OSFXSR) {
        fpu->has_sse = 1;
        cr4 |= FPU_CR4_OSXMMEXCPT;
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }
    return fpu->present ? 0 : -1;
}
void fpu_save_state(void *state)
{
    asm volatile("fnsave %0" : "=m"(*(char *)state));
}
void fpu_restore_state(void *state)
{
    asm volatile("frstor %0" : : "m"(*(char *)state));
}
