#ifndef MDA_H
#define MDA_H
#include <stdint.h>
#define MDA_BASE 0x3B0
#define MDA_CRTC_ADDR 0x3B4
#define MDA_CRTC_DATA 0x3B5
#define MDA_STATUS 0x3BA
#define MDA_FB 0xB0000
#define MDA_COLS 80
#define MDA_ROWS 25
#define MDA_ATTR_BLINK 0x80
#define MDA_ATTR_HIGH 0x08
#define MDA_ATTR_UNDERLINE 0x01
typedef struct {
    int present;
} mda_device_t;
int mda_init(mda_device_t *dev);
void mda_write_char(mda_device_t *dev, int row, int col, char c, uint8_t attr);
#endif
