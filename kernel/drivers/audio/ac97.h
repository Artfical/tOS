#ifndef AC97_H
#define AC97_H
#include <stdint.h>
#define AC97_RESET 0x00
#define AC97_MASTER_VOL 0x02
#define AC97_PCM_OUT 0x18
#define AC97_EXT_AUDIO 0x28
#define AC97_EXT_AUDIO_VRA 0x0001
typedef struct {
    int present;
    uint16_t io_base;
    uint32_t nambar;
    uint32_t nabmbar;
    int stereo;
    int sample_rate;
} ac97_device_t;
int ac97_init(ac97_device_t *dev);
void ac97_write(ac97_device_t *dev, uint8_t reg, uint16_t val);
uint16_t ac97_read(ac97_device_t *dev, uint8_t reg);
#endif
