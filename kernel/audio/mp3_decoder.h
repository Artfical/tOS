#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <stdint.h>

/* Maximum decoded PCM samples per MP3 frame (1152) */
#define MP3_FRAME_SAMPLES 1152

typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;         /* byte position in data */

    /* frame header (last parsed) */
    uint32_t sample_rate;
    uint8_t  channels;         /* 1 or 2 */
    uint32_t bitrate;

    /* per-channel overlap buffer for IMDCT */
    int32_t  overlap[2][18*32];

    /* polyphase synthesis V buffer */
    int32_t  V[2][1024];
    int      V_off[2];

    /* total decoded samples (mono-equivalent) */
    uint32_t total_samples;
    uint32_t cur_sample;

    int      valid;
} mp3_ctx_t;

/* Open from fully-loaded buffer. Returns 0 on success. */
int      mp3_open(mp3_ctx_t *ctx, const uint8_t *data, uint32_t size);

/* Decode one MP3 frame into pcm_out[0..MP3_FRAME_SAMPLES-1] (signed 16-bit).
   Returns number of samples written (0 at EOF or unrecoverable error). */
uint32_t mp3_decode_frame(mp3_ctx_t *ctx, int16_t *pcm_out);

/* Fill out[] with up to out_len samples (8-bit unsigned, mono, 22050 Hz).
   Returns bytes written; 0 at EOF. */
uint32_t mp3_read(mp3_ctx_t *ctx, uint8_t *out, uint32_t out_len);

void     mp3_seek_sec(mp3_ctx_t *ctx, uint32_t sec);
uint32_t mp3_duration_sec(const mp3_ctx_t *ctx);
uint32_t mp3_pos_sec(const mp3_ctx_t *ctx);

#endif
