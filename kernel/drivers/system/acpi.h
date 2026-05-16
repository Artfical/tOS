#ifndef ACPI_H
#define ACPI_H
#include <stdint.h>
#define ACPI_RSDP_SIGNATURE "RSD PTR "
#define ACPI_EBDA_START 0x000E0000
#define ACPI_EBDA_END 0x000FFFFF
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_addr;
} __attribute__((packed)) acpi_rsdp_t;
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_tableid[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;
typedef struct {
    acpi_sdt_t header;
    uint32_t entries[];
} __attribute__((packed)) acpi_rsdt_t;
typedef struct {
    acpi_sdt_t header;
    uint8_t hardware_id;
    uint8_t flags;
    uint32_t evt_block_addr;
    uint32_t ctl_block_addr;
} __attribute__((packed)) acpi_fadt_t;
typedef struct {
    acpi_sdt_t header;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;
int acpi_init(void);
void *acpi_find_table(const char *signature);
uint32_t acpi_get_lapic_addr(void);
#endif
