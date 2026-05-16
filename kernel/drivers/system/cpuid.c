#include "cpuid.h"
#include "string.h"
int cpuid_string(int code, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    int has = 0;
    asm volatile("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; pushl %1; popfl" : "=r"(has), "=r"(code));
    (void)code;
    if (!(has & 0x200000)) return -1;
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(code));
    return 0;
}
int cpuid_init(cpu_info_t *info)
{
    int has = 0;
    asm volatile("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; pushl %1; popfl" : "=r"(has), "=r"(info->has_cpuid));
    uint32_t a, b, c, d;
    if (cpuid_string(0, &a, &b, &c, &d)) {
        info->has_cpuid = 0;
        return -1;
    }
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
