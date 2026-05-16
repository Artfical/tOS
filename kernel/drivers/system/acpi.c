#include "acpi.h"
#include "string.h"
static acpi_rsdp_t *acpi_find_rsdp(void)
{
    for (uint32_t addr = ACPI_EBDA_START; addr < ACPI_EBDA_END; addr += 16) {
        acpi_rsdp_t *rsdp = (acpi_rsdp_t *)addr;
        if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
            rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ') {
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += ((uint8_t *)rsdp)[i];
            if (sum == 0) return rsdp;
        }
    }
    return NULL;
}
static acpi_rsdt_t *acpi_rsdt = NULL;
int acpi_init(void)
{
    acpi_rsdp_t *rsdp = acpi_find_rsdp();
    if (!rsdp) return -1;
    acpi_rsdt = (acpi_rsdt_t *)(uint32_t)rsdp->rsdt_addr;
    return 0;
}
void *acpi_find_table(const char *signature)
{
    if (!acpi_rsdt) return NULL;
    int entries = (acpi_rsdt->header.length - sizeof(acpi_sdt_t)) / 4;
    for (int i = 0; i < entries; i++) {
        acpi_sdt_t *sdt = (acpi_sdt_t *)(uint32_t)acpi_rsdt->entries[i];
        if (sdt->signature[0] == signature[0] && sdt->signature[1] == signature[1] &&
            sdt->signature[2] == signature[2] && sdt->signature[3] == signature[3])
            return sdt;
    }
    return NULL;
}
uint32_t acpi_get_lapic_addr(void)
{
    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");
    if (!madt) return 0;
    return madt->lapic_addr;
}
