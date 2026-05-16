#ifndef HDA_H
#define HDA_H
#include <stdint.h>
#define HDA_ICW 0x60
#define HDA_IRR 0x64
#define HDA_ICS 0x68
#define HDA_ICW_BUSY 0x80000000
#define HDA_CORB_BASE 0x40
#define HDA_RIRB_BASE 0x50
#define HDA_STATESTS 0x0E0
#define HDA_INTCTL 0x20
#define HDA_INTSTS 0x24
#define HDA_WAKEEN 0x0E8
#define HDA_GCAP_NSDO(n) (((n) >> 14) & 0x03)
#define HDA_GCAP_OSS(n)  (((n) >> 12) & 0x0F)
#define HDA_GCAP_ISS(n)  (((n) >> 8) & 0x0F)
typedef struct {
    uint32_t mmio_base;
    int present;
    int output_streams;
    int input_streams;
    int bidir_streams;
} hda_controller_t;
int hda_init(hda_controller_t *dev);
int hda_send_command(hda_controller_t *dev, uint32_t cmd);
uint32_t hda_read_response(hda_controller_t *dev);
#endif
