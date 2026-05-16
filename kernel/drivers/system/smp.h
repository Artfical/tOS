#ifndef SMP_H
#define SMP_H
#include <stdint.h>
#define SMP_MAX_CPUS 16
typedef struct {
    int cpu_count;
    uint8_t apic_ids[SMP_MAX_CPUS];
    uint32_t lapic_base;
} smp_info_t;
int smp_init(smp_info_t *info);
int smp_start_aps(smp_info_t *info);
#endif
