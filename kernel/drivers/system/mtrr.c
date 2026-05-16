#include "mtrr.h"
#include "msr.h"
#include "cpuid.h"
int mtrr_init(mtrr_info_t *info)
{
    info->present = 0;
    uint32_t a, b, c, d;
    if (cpuid_string(1, &a, &b, &c, &d)) return -1;
    (void)a;
    (void)b;
    if (!(d & (1 << 12))) return -1;
    uint64_t def = msr_read64(MSR_MTRR_DEF_TYPE);
    info->present = !!(def & (1 << 11));
    uint32_t eax = 0;
    if (cpuid_string(0x80000008, &eax, &b, &c, &d) == 0)
        info->var_count = eax & 0xFF;
    else
        info->var_count = 8;
    return 0;
}
int mtrr_set_var(int index, uint64_t base, uint64_t mask, int type)
{
    msr_write64(MSR_MTRR_PHYS_BASE0 + index * 2, base | type);
    msr_write64(MSR_MTRR_PHYS_MASK0 + index * 2, mask | (1 << 11));
    return 0;
}
