#ifndef SMBIOS_H
#define SMBIOS_H
#include <stdint.h>
#define SMBIOS_ENTRY_POINT 0x000F0000
#define SMBIOS_SEARCH_SIZE 0x00010000
typedef struct {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed)) smbios_header_t;
typedef struct {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
    uint8_t manufacturer;
    uint8_t product;
    uint8_t version;
    uint8_t serial;
    uint8_t uuid[16];
    uint8_t wakeup_type;
} __attribute__((packed)) smbios_system_info_t;
typedef struct {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
    uint8_t socket;
    uint8_t processor_type;
    uint8_t processor_family;
    uint8_t manufacturer;
    uint64_t max_speed;
    uint64_t current_speed;
} __attribute__((packed)) smbios_processor_info_t;
int smbios_init(void);
smbios_header_t *smbios_find_entry(uint8_t type);
const char *smbios_get_string(smbios_header_t *header, uint8_t index);
#endif
