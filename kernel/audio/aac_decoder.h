#ifndef AAC_DECODER_H
#define AAC_DECODER_H

#include <stdint.h>

/* AAC LC ADTS decoder -- long blocks only, mono/stereo, no SBR/PS */

#define AAC_FRAME_LEN    1024   /* spectral lines per channel per frame */
#define AAC_MAX_SFB      51     /* max scalefactor bands (long block) */
#define AAC_MAX_CH       2

typedef struct {
    /* public (set by aac_open) */
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;          /* byte offset into data */
    uint32_t       sample_rate;
    uint8_t        channels;
    int            valid;

    /* internal */
    uint32_t       total_samples;
    uint32_t       cur_sample;

    /* overlap buffer for IMDCT (one per channel) */
    int32_t  overlap[AAC_MAX_CH][AAC_FRAME_LEN];

    /* sample-rate output resampling state */
    uint32_t rs_frac;

    /* intermediate PCM (signed 16-bit, mixed to mono before output) */
    int16_t  pcm[AAC_FRAME_LEN];
    uint32_t pcm_fill;   /* valid samples in pcm[] */
    uint32_t pcm_pos;    /* next sample to consume */

    /* tables init flag */
    int      tables_ready;
} aac_ctx_t;

int      aac_open(aac_ctx_t *ctx, const uint8_t *data, uint32_t size);
uint32_t aac_read(aac_ctx_t *ctx, uint8_t *out, uint32_t out_len);
uint32_t aac_duration_sec(const aac_ctx_t *ctx);
uint32_t aac_pos_sec(const aac_ctx_t *ctx);

#endif
