#include "aac_decoder.h"
#include "string.h"

/* AAC/M4A decoder – LC-AAC profile stub.
   Full MDCT + spectral decoding is a future implementation.
   Returns a user-visible error rather than silent failure. */

int aac_open(aac_ctx_t *ctx, const uint8_t *data, uint32_t size)
{
    if (!data || size < 8) return -1;
    memset(ctx, 0, sizeof(*ctx));

    /* Detect ADTS sync word (0xFFF or 0xFFE) */
    for (uint32_t i = 0; i + 4 <= size; i++) {
        if ((data[i] == 0xFF) && ((data[i+1] & 0xF0) == 0xF0)) {
            /* ADTS header found: parse sample rate index */
            static const uint32_t sr_idx[] = {
                96000,88200,64000,48000,44100,32000,
                24000,22050,16000,12000,11025,8000,7350
            };
            uint8_t prot = (data[i+1] >> 0) & 1;  /* 0=CRC, 1=no CRC */
            uint8_t sridx = (data[i+2] >> 2) & 0xF;
            uint8_t ch    = ((data[i+2] & 1) << 2) | ((data[i+3] >> 6) & 3);
            (void)prot;
            ctx->data = data;
            ctx->size = size;
            ctx->pos  = i;
            ctx->sample_rate = (sridx < 13) ? sr_idx[sridx] : 44100;
            ctx->channels    = ch ? ch : 2;
            ctx->valid = 0; /* mark as "detected but not decodable" */
            return -2;      /* -2 = format detected, decoder not available */
        }
    }
    /* Try MP4/M4A container (ftyp box) */
    if (data[4]=='f' && data[5]=='t' && data[6]=='y' && data[7]=='p') {
        memset(ctx, 0, sizeof(*ctx));
        ctx->data = data; ctx->size = size;
        return -2;
    }
    return -1;
}

uint32_t aac_read(aac_ctx_t *ctx, uint8_t *out, uint32_t out_len)
{
    (void)ctx; (void)out; (void)out_len;
    return 0;
}

uint32_t aac_duration_sec(const aac_ctx_t *ctx) { (void)ctx; return 0; }
uint32_t aac_pos_sec(const aac_ctx_t *ctx)       { (void)ctx; return 0; }
