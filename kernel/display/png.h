#ifndef PNG_DECODE_H
#define PNG_DECODE_H

#include <stdint.h>

int png_decode(const uint8_t *file, uint32_t file_len, uint8_t **out_rgb,
                uint32_t *out_w, uint32_t *out_h, char *err, int err_cap);

#endif
