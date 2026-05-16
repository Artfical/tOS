#ifndef ISA_H
#define ISA_H

#include <stdint.h>

#define ISA_IO_BASE 0x0000
#define ISA_IO_SIZE 0x0400
#define ISA_MEM_BASE 0x000A0000
#define ISA_MEM_SIZE 0x00060000

#define ISA_DMA1 0x00
#define ISA_DMA2 0x80
#define ISA_PIC1 0x20
#define ISA_PIC2 0xA0
#define ISA_PIT 0x40
#define ISA_PS2 0x60
#define ISA_CMOS 0x70

typedef struct {
    uint16_t io_base;
    uint8_t irq;
    uint8_t dma;
} isa_device_t;

int isa_detect_device(isa_device_t *dev, uint16_t io_port, uint8_t expected_id);
void isa_system_reset(void);

#endif
