#include "ac97.h"
#include "pci.h"
#include "io.h"
uint16_t ac97_read(ac97_device_t *dev, uint8_t reg)
{
    outl(dev->nambar + 0x00, reg);
    return inl(dev->nambar + 0x04);
}
void ac97_write(ac97_device_t *dev, uint8_t reg, uint16_t val)
{
    outl(dev->nambar + 0x00, reg);
    outl(dev->nambar + 0x04, val);
}
int ac97_init(ac97_device_t *dev)
{
    pci_device_t pci_devs[4];
    int n = pci_find_devices(0x04, 0x01, pci_devs, 4);
    if (!n) return -1;
    dev->present = 0;
    dev->nambar = pci_get_bar(pci_devs[0].bus, pci_devs[0].device, pci_devs[0].func, 0) & 0xFFFFFFF0;
    dev->nabmbar = pci_get_bar(pci_devs[0].bus, pci_devs[0].device, pci_devs[0].func, 1) & 0xFFFFFFF0;
    if (!dev->nambar || !dev->nabmbar) return -1;
    ac97_write(dev, AC97_RESET, 0);
    ac97_write(dev, AC97_MASTER_VOL, 0x0000);
    ac97_write(dev, AC97_PCM_OUT, 0x0000);
    uint16_t ext = ac97_read(dev, AC97_EXT_AUDIO);
    if (ext & AC97_EXT_AUDIO_VRA) {
        ac97_write(dev, AC97_EXT_AUDIO, ext | AC97_EXT_AUDIO_VRA);
    }
    dev->stereo = 1;
    dev->sample_rate = 48000;
    dev->present = 1;
    return 0;
}
