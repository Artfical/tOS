#ifndef AAC_DECODER_H
#define AAC_DECODER_H

#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;
    uint32_t       sample_rate;
    uint8_t        channels;
    int            valid;
} aac_ctx_t;

/* Returns 0 if the buffer looks like ADTS AAC; fills ctx fields.
   Currently returns -2 ("not implemented") always. */
int      aac_open(aac_ctx_t *ctx, const uint8_t *data, uint32_t size);
uint32_t aac_read(aac_ctx_t *ctx, uint8_t *out, uint32_t out_len);
uint32_t aac_duration_sec(const aac_ctx_t *ctx);
uint32_t aac_pos_sec(const aac_ctx_t *ctx);

#endif
