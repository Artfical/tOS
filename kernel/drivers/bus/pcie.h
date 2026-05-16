#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

#define PCIE_CONFIG_ADDR 0xCF8
#define PCIE_CONFIG_DATA 0xCFC

#define PCIE_CAP_ID 0x10
#define PCIE_CAP_NEXT 0x11
#define PCIE_CAP_EXP 0x12

#define PCIE_TYPE_ENDPOINT 0
#define PCIE_TYPE_ROOT_PORT 4
#define PCIE_TYPE_UPSTREAM 5
#define PCIE_TYPE_DOWNSTREAM 6
#define PCIE_TYPE_RC_EC 7

typedef struct {
    uint16_t cap_id;
    uint8_t cap_version;
    uint8_t device_type;
    uint8_t link_speed;
    uint8_t link_width;
    uint8_t slot_power;
} pcie_cap_t;

int pcie_find_capability(uint8_t bus, uint8_t device, uint8_t func, pcie_cap_t *cap);
int pcie_detect(void);
uint32_t pcie_mmio_read(uint32_t mmio_base, uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

#endif
