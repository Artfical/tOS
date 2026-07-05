#ifndef BOCHS_H
#define BOCHS_H
#include <stdint.h>
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_BANK 5
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9
#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_GETCAPS 0x02
#define VBE_DISPI_8BIT_DAC 0x20
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80
#define VBE_DISPI_ID4 0xB0C4
#define VBE_DISPI_ID5 0xB0C5
typedef struct {
    int present;
    int width;
    int height;
    int bpp;
    uint32_t lfb;
} bochs_device_t;
int bochs_init(bochs_device_t *dev);
int bochs_set_mode(bochs_device_t *dev, int width, int height, int bpp);
void bochs_put_pixel(bochs_device_t *dev, int x, int y, uint32_t color);

/* Turns VBE back off. Unlike hand-rolled legacy VGA mode switching
 * (CRTC/Sequencer/Graphics Controller/Attribute Controller register
 * gymnastics -- fragile enough that a from-scratch mode 13h attempt
 * here left the screen corrupted on the way back to text mode), VBE
 * is a strict overlay on top of the legacy VGA state: disabling it
 * cleanly falls back to whatever text mode was already active
 * underneath, no manual register restore needed. */
void bochs_disable(void);
#endif
