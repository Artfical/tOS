#ifndef VESA_H
#define VESA_H
#include <stdint.h>
#define VESA_SIGNATURE 0x41534556
#define VESA_MAGIC 0x4F
#define VESA_FUNC_INFO 0x4F00
#define VESA_FUNC_MODE 0x4F01
#define VESA_FUNC_SET_MODE 0x4F02
#define VESA_MODE_LINEAR (1 << 14)
#define VESA_MODE_1024x768 0x117
#define VESA_MODE_800x600 0x115
#define VESA_MODE_640x480 0x101
typedef struct {
    uint8_t signature[4];
    uint16_t version;
    uint32_t oem;
    uint32_t capabilities;
    uint32_t video_modes;
    uint16_t total_memory;
} __attribute__((packed)) vesa_info_t;
typedef struct {
    uint16_t mode_attr;
    uint8_t win_a_attr;
    uint8_t win_b_attr;
    uint16_t win_granularity;
    uint16_t win_size;
    uint16_t win_a_seg;
    uint16_t win_b_seg;
    uint32_t win_func;
    uint16_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t w_char;
    uint8_t y_char;
    uint8_t planes;
    uint8_t bpp;
    uint8_t banks;
    uint8_t mem_model;
    uint8_t bank_size;
    uint8_t image_pages;
    uint8_t reserved0;
    uint8_t red_mask;
    uint8_t red_pos;
    uint8_t green_mask;
    uint8_t green_pos;
    uint8_t blue_mask;
    uint8_t blue_pos;
    uint8_t rsvd_mask;
    uint8_t rsvd_pos;
    uint8_t directcolor_info;
    uint32_t phys_base;
    uint32_t reserved1;
    uint16_t reserved2;
} __attribute__((packed)) vesa_mode_info_t;
int vesa_init(void);
int vesa_set_mode(uint16_t mode);
int vesa_get_info(vesa_info_t *info);
int vesa_get_mode_info(uint16_t mode, vesa_mode_info_t *info);
#endif
