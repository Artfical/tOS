#ifndef BIOS32_H
#define BIOS32_H
#include <stdint.h>
#define BIOS32_SIGNATURE "_32_"
#define BIOS32_SERVICE_PCI 0x01
#define BIOS32_SERVICE_PNP 0x0A
typedef struct {
    char signature[4];
    uint32_t entry;
    uint8_t revision;
    uint8_t length;
    uint8_t checksum;
    uint8_t reserved[5];
} __attribute__((packed)) bios32_t;
int bios32_init(void);
void *bios32_find_service(uint32_t service);
#endif
