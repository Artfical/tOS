#ifndef TFIRE_IMAGES_H
#define TFIRE_IMAGES_H

/* SVG and WebP pictures (images.c); PNG and JPEG are tOS's own (uihost_image_decode) */

#include <stdint.h>

enum { TF_IMG_OTHER, TF_IMG_SVG, TF_IMG_WEBP };

int tf_image_kind(const uint8_t *d, uint32_t n);
/* an SVG drawn into RGBA (malloc'd): at about want_w x want_h if given (0 = its own size), at most max; 0 = ok */
int tf_svg_decode(const uint8_t *d, uint32_t n, int want_w, int want_h, int max_w, int max_h, uint8_t **rgba, int *w, int *h);
int tf_webp_decode(const uint8_t *d, uint32_t n, int max_w, int max_h, uint8_t **rgba, int *w, int *h);

#endif
