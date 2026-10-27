#include "fpu.h"
#include "cpuid.h"
#include "string.h"

static fpu_device_t fpu_caps;
static uint8_t initial_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
static int initial_state_ready;

int fpu_init(fpu_device_t *fpu)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint32_t cr0, cr4;

    if (!fpu) return -1;
    memset(fpu, 0, sizeof(*fpu));
    memset(initial_state, 0, sizeof(initial_state));
    initial_state_ready = 0;

    if (cpuid_string(1, &a, &b, &c, &d) != 0)
        return -1;

    fpu->present = (d & (1u << CPUID_FEATURE_EDX_FPU)) != 0;
    fpu->has_fxsave = (d & (1u << CPUID_FEATURE_EDX_FXSR)) != 0;
    fpu->has_sse = (d & (1u << CPUID_FEATURE_EDX_SSE)) != 0;
    fpu->has_sse2 = (d & (1u << CPUID_FEATURE_EDX_SSE2)) != 0;
    if (!fpu->has_fxsave) {
        fpu->has_sse = 0;
        fpu->has_sse2 = 0;
    }
    fpu_caps = *fpu;
    if (!fpu->present) return -1;

    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(FPU_CR0_EM | FPU_CR0_TS);
    cr0 |= FPU_CR0_MP | FPU_CR0_NE;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (fpu->has_fxsave) {
        cr4 |= FPU_CR4_OSFXSR;
        if (fpu->has_sse) cr4 |= FPU_CR4_OSXMMEXCPT;
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    asm volatile("fninit");
    if (fpu->has_sse) {
        uint32_t mxcsr = 0x1F80; /* architectural reset value */
        asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    }

    if (fpu->has_fxsave) {
        asm volatile("fxsave (%0)" : : "r"(initial_state) : "memory");
    } else {
        asm volatile("fnsave (%0)" : : "r"(initial_state) : "memory");
        asm volatile("frstor (%0)" : : "r"(initial_state) : "memory");
    }
    initial_state_ready = 1;
    return 0;
}

void fpu_state_init(void *state)
{
    if (!state) return;
    if (initial_state_ready) memcpy(state, initial_state, FPU_STATE_SIZE);
    else memset(state, 0, FPU_STATE_SIZE);
}

void fpu_save_state(void *state)
{
    if (!state || !fpu_caps.present) return;
    if (fpu_caps.has_fxsave)
        asm volatile("fxsave (%0)" : : "r"(state) : "memory");
    else
        asm volatile("fnsave (%0)" : : "r"(state) : "memory");
}

void fpu_restore_state(void *state)
{
    if (!state || !fpu_caps.present) return;
    if (fpu_caps.has_fxsave)
        asm volatile("fxrstor (%0)" : : "r"(state) : "memory");
    else
        asm volatile("frstor (%0)" : : "r"(state) : "memory");
}

int fpu_has_sse2(void)
{
    return fpu_caps.has_sse2;
}
