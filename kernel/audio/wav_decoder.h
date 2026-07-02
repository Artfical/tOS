#ifndef WAV_DECODER_H
#define WAV_DECODER_H

#include <stdint.h>

typedef struct {
    const uint8_t *data;    /* full file in memory */
    uint32_t       size;    /* file size in bytes  */
    uint32_t       pos;     /* current read position inside 'data' */

    /* parsed header */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;          /* 8 or 16 */
    uint32_t data_offset;   /* byte offset of first PCM sample in data[] */
    uint32_t data_size;     /* PCM bytes */
    uint32_t total_samples; /* per channel */
} wav_ctx_t;

/* Initialise decoder from a fully-loaded buffer. Returns 0 on success. */
int wav_open(wav_ctx_t *ctx, const uint8_t *data, uint32_t size);

/* Decode up to 'out_len' samples into 'out' (8-bit unsigned, mono, 22050 Hz).
   Returns number of samples written, 0 at EOF. */
uint32_t wav_read(wav_ctx_t *ctx, uint8_t *out, uint32_t out_len);

/* Seek to sample position (resampled). */
void wav_seek(wav_ctx_t *ctx, uint32_t sample_pos);

/* Total duration in seconds (resampled to 22050 Hz). */
uint32_t wav_duration_sec(const wav_ctx_t *ctx);

/* Current position in seconds (resampled). */
uint32_t wav_pos_sec(const wav_ctx_t *ctx);

#endif
