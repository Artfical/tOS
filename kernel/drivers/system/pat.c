#include "pat.h"
#include "msr.h"
#include "cpuid.h"
int pat_init(pat_info_t *info)
{
    uint32_t a, b, c, d;
    if (cpuid_string(1, &a, &b, &c, &d)) return -1;
    (void)a;
    (void)b;
    info->present = !!(d & (1 << 16));
    if (info->present) {
        info->pat_value = msr_read64(MSR_PAT);
    }
    return info->present ? 0 : -1;
}
void pat_set_entry(int entry, int type)
{
    uint64_t pat = msr_read64(MSR_PAT);
    pat &= ~(7ULL << (entry * 8));
    pat |= (uint64_t)(type & 7) << (entry * 8);
    msr_write64(MSR_PAT, pat);
}
