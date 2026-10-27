#include "cpuid.h"
#include "string.h"
int cpuid_string(int code, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t flags_before, flags_after;
    asm volatile(
        "pushfl; popl %0; "
        "movl %0, %1; xorl $0x200000, %1; "
        "pushl %1; popfl; pushfl; popl %1; "
        "pushl %0; popfl"
        : "=&r"(flags_before), "=&r"(flags_after)
        :
        : "cc");
    if (((flags_before ^ flags_after) & 0x200000u) == 0) return -1;

    uint32_t max_leaf, leaf0_b, leaf0_c, leaf0_d;
    asm volatile("cpuid"
        : "=a"(max_leaf), "=b"(leaf0_b), "=c"(leaf0_c), "=d"(leaf0_d)
        : "a"(0));
    if ((uint32_t)code > max_leaf) return -1;
    if (code == 0) {
        *a = max_leaf;
        *b = leaf0_b;
        *c = leaf0_c;
        *d = leaf0_d;
    } else {
        asm volatile("cpuid"
            : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
            : "a"((uint32_t)code)
            : "memory");
    }
    return 0;
}
int cpuid_init(cpu_info_t *info)
{
    uint32_t a, b, c, d;
    memset(info, 0, sizeof(*info));
    if (cpuid_string(0, &a, &b, &c, &d)) {
        info->has_cpuid = 0;
        return -1;
    }
    info->has_cpuid = 1;
    memcpy(info->vendor, &b, 4);
    memcpy(info->vendor + 4, &d, 4);
    memcpy(info->vendor + 8, &c, 4);
    info->vendor[12] = 0;
    if (cpuid_string(1, &a, &b, &c, &d)) return -1;
    info->stepping = a & 0x0F;
    info->model = (a >> 4) & 0x0F;
    info->family = (a >> 8) & 0x0F;
    info->ext_model = (a >> 16) & 0x0F;
    info->ext_family = (a >> 20) & 0xFF;
    info->features_ecx = c;
    info->features_edx = d;
    return 0;
}
