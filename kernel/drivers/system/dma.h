#ifndef DMA_H
#define DMA_H
#include <stdint.h>
#define DMA1_BASE 0x00
#define DMA2_BASE 0xC0
#define DMA1_CHAN0 0x87
#define DMA1_CHAN1 0x83
#define DMA1_CHAN2 0x81
#define DMA1_CHAN3 0x82
#define DMA2_CHAN4 0x8F
#define DMA2_CHAN5 0x8B
#define DMA2_CHAN6 0x89
#define DMA2_CHAN7 0x8A
#define DMA_MODE_READ 0x44
#define DMA_MODE_WRITE 0x48
#define DMA_MODE_SINGLE 0x40
#define DMA_MODE_AUTO 0x50
#define DMA_MODE_DEMAND 0x00
#define DMA_MODE_CASCADE 0xC0
void dma_init(void);
void dma_set_address(uint8_t channel, uint32_t addr);
void dma_set_count(uint8_t channel, uint32_t count);
void dma_set_mode(uint8_t channel, uint8_t mode);
void dma_start(uint8_t channel);
void dma_reset(int dma);
#endif
