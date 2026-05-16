#ifndef FPU_H
#define FPU_H
#include <stdint.h>
#define FPU_CR0_EM (1 << 2)
#define FPU_CR0_MP (1 << 1)
#define FPU_CR0_NE (1 << 5)
#define FPU_CR0_TS (1 << 3)
#define FPU_CR4_OSFXSR (1 << 9)
#define FPU_CR4_OSXMMEXCPT (1 << 10)
typedef struct {
    int present;
    int has_sse;
} fpu_device_t;
int fpu_init(fpu_device_t *fpu);
void fpu_save_state(void *state);
void fpu_restore_state(void *state);
#endif
