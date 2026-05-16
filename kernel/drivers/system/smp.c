#include "smp.h"
#include "apic.h"
#include "acpi.h"
int smp_init(smp_info_t *info)
{
    info->cpu_count = 1;
    info->apic_ids[0] = 0;
    info->lapic_base = acpi_get_lapic_addr();
    if (!info->lapic_base) info->lapic_base = 0xFEE00000;
    return 0;
}
int smp_start_aps(smp_info_t *info)
{
    (void)info;
    return -1;
}
