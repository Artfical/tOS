#ifndef CPUID_H
#define CPUID_H
#include <stdint.h>
#define CPUID_VENDOR_INTEL "GenuineIntel"
#define CPUID_VENDOR_AMD "AuthenticAMD"
#define CPUID_FEATURE_ECX_SSE3 0
#define CPUID_FEATURE_ECX_SSSE3 9
#define CPUID_FEATURE_ECX_SSE4_1 19
#define CPUID_FEATURE_ECX_SSE4_2 20
#define CPUID_FEATURE_ECX_AES 25
#define CPUID_FEATURE_ECX_AVX 28
#define CPUID_FEATURE_EDX_FPU 0
#define CPUID_FEATURE_EDX_MMX 23
#define CPUID_FEATURE_EDX_SSE 25
#define CPUID_FEATURE_EDX_SSE2 26
#define CPUID_FEATURE_EDX_APIC 9
typedef struct {
    char vendor[13];
    uint8_t stepping;
    uint8_t model;
    uint8_t family;
    uint8_t ext_model;
    uint8_t ext_family;
    uint32_t features_ecx;
    uint32_t features_edx;
    int has_cpuid;
} cpu_info_t;
int cpuid_init(cpu_info_t *info);
int cpuid_string(int code, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);
#endif
