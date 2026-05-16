#ifndef SB16_H
#define SB16_H
#include <stdint.h>
#define SB16_DSP_RESET 0x226
#define SB16_DSP_READ 0x22A
#define SB16_DSP_WRITE 0x22C
#define SB16_DSP_DATA_AVAIL 0x22E
#define SB16_MIXER_ADDR 0x224
#define SB16_MIXER_DATA 0x225
#define SB16_CMD_SET_TIME_CONSTANT 0x40
#define SB16_CMD_SET_OUTPUT_FREQ 0x41
#define SB16_CMD_DSP_VERSION 0xE1
#define SB16_CMD_SPEAKER_ON 0xD1
#define SB16_CMD_SPEAKER_OFF 0xD3
#define SB16_CMD_8BIT_PLAY 0x14
#define SB16_CMD_8BIT_PLAY_LOOP 0x1C
#define SB16_CMD_16BIT_PLAY 0xB0
#define SB16_CMD_16BIT_PLAY_LOOP 0xB6
#define SB16_CMD_STOP_8BIT 0xD0
#define SB16_CMD_STOP_16BIT 0xD5
#define SB16_CMD_CONTINUE_8BIT 0xD4
#define SB16_CMD_PAUSE_16BIT 0xD5
typedef struct {
    int present;
    uint16_t base;
    uint8_t irq;
    uint8_t dma8;
    uint8_t dma16;
    uint8_t major_ver;
    uint8_t minor_ver;
} sb16_device_t;
int sb16_init(sb16_device_t *dev);
int sb16_reset(sb16_device_t *dev);
void sb16_write(sb16_device_t *dev, uint8_t val);
uint8_t sb16_read(sb16_device_t *dev);
#endif
