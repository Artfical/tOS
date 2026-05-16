#include "dma.h"
#include "io.h"
void dma_init(void)
{
    outb(0x0A, 0x04);
    outb(0xD4, 0x04);
}
void dma_set_address(uint8_t channel, uint32_t addr)
{
    (void)channel;
    (void)addr;
}
void dma_set_count(uint8_t channel, uint32_t count)
{
    (void)channel;
    (void)count;
}
void dma_set_mode(uint8_t channel, uint8_t mode)
{
    (void)channel;
    (void)mode;
}
void dma_start(uint8_t channel)
{
    (void)channel;
}
void dma_reset(int dma)
{
    outb(dma == 1 ? 0x0A : 0xD4, 0x06);
    outb(dma == 1 ? 0xD4 : 0x0A, 0x07);
    outb(dma == 1 ? 0xD8 : 0x02, 0xFF);
}
