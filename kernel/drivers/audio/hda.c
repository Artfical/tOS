#include "hda.h"
#include "pci.h"
#include "io.h"
int hda_send_command(hda_controller_t *dev, uint32_t cmd)
{
    volatile uint32_t *base = (volatile uint32_t *)dev->mmio_base;
    for (int i = 0; i < 100000; i++)
        if (!(base[HDA_ICS / 4] & HDA_ICW_BUSY)) break;
    base[HDA_ICW / 4] = cmd;
    base[HDA_ICS / 4] = 0;
    for (int i = 0; i < 100000; i++)
        if (!(base[HDA_ICS / 4] & HDA_ICW_BUSY)) return 0;
    return -1;
}
uint32_t hda_read_response(hda_controller_t *dev)
{
    volatile uint32_t *base = (volatile uint32_t *)dev->mmio_base;
    return base[HDA_IRR / 4];
}
int hda_init(hda_controller_t *dev)
{
    pci_device_t pci_devs[4];
    int n = pci_find_devices(0x04, 0x03, pci_devs, 4);
    if (!n) return -1;
    uint32_t bar = pci_get_bar(pci_devs[0].bus, pci_devs[0].device, pci_devs[0].func, 0);
    dev->mmio_base = bar & 0xFFFFFFF0;
    volatile uint32_t *base = (volatile uint32_t *)dev->mmio_base;
    uint16_t gcap = base[0] & 0xFFFF;
    dev->output_streams = HDA_GCAP_OSS(gcap);
    dev->input_streams = HDA_GCAP_ISS(gcap);
    dev->bidir_streams = HDA_GCAP_NSDO(gcap);
    base[HDA_INTCTL / 4] = 0;
    base[HDA_WAKEEN / 4] = 0;
    dev->present = 1;
    return 0;
}
