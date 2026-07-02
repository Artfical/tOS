#include "wav_decoder.h"
#include "audio.h"  /* AUDIO_OUT_RATE */
#include "string.h"

/* ── Little-endian helpers ─────────────────────────────────────────────── */
static uint16_t ru16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t ru32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ── RIFF/WAV chunk scanner ─────────────────────────────────────────────── */
int wav_open(wav_ctx_t *ctx, const uint8_t *data, uint32_t size)
{
    if (!data || size < 44) return -1;
    /* 'RIFF' */
    if (data[0]!='R'||data[1]!='I'||data[2]!='F'||data[3]!='F') return -1;
    if (data[8]!='W'||data[9]!='A'||data[10]!='V'||data[11]!='E') return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->data = data;
    ctx->size = size;

    uint32_t off = 12;
    uint32_t fmt_found = 0, data_found = 0;

    while (off + 8 <= size) {
        uint32_t csize = ru32(data + off + 4);
        if (data[off]=='f'&&data[off+1]=='m'&&data[off+2]=='t'&&data[off+3]==' ') {
            if (csize < 16) return -1;
            if (ru16(data + off + 8) != 1) return -1; /* PCM only */
            ctx->channels    = ru16(data + off + 10);
            ctx->sample_rate = ru32(data + off + 12);
            ctx->bits        = ru16(data + off + 22);
            fmt_found = 1;
        } else if (data[off]=='d'&&data[off+1]=='a'&&data[off+2]=='t'&&data[off+3]=='a') {
            ctx->data_offset = off + 8;
            ctx->data_size   = csize;
            data_found = 1;
        }
        off += 8 + csize;
        if (csize & 1) off++; /* RIFF padding */
    }

    if (!fmt_found || !data_found) return -1;
    if (ctx->channels < 1 || ctx->channels > 2) return -1;
    if (ctx->bits != 8 && ctx->bits != 16) return -1;

    uint32_t bps = (uint32_t)ctx->channels * (ctx->bits >> 3);
    ctx->total_samples = (bps > 0) ? (ctx->data_size / bps) : 0;
    ctx->pos = ctx->data_offset;
    return 0;
}

/* ── Decode + resample to AUDIO_OUT_RATE, 8-bit unsigned, mono ─────────── */
uint32_t wav_read(wav_ctx_t *ctx, uint8_t *out, uint32_t out_len)
{
    if (!ctx->data || !out_len) return 0;

    uint32_t written = 0;
    uint32_t src_rate = ctx->sample_rate;
    uint32_t src_bps  = (uint32_t)ctx->channels * (ctx->bits >> 3); /* bytes/frame */
    uint32_t out_rate = AUDIO_OUT_RATE;

    /* Use fixed-point ratio: advance src by (src_rate/out_rate) each out sample */
    /* Q16: ratio = src_rate * 65536 / out_rate */
    uint32_t step_q16 = (uint32_t)((uint64_t)src_rate * 65536U / out_rate);
    static uint32_t frac_q16 = 0; /* fractional src position accumulator */

    uint32_t data_end = ctx->data_offset + ctx->data_size;
    if (data_end > ctx->size) data_end = ctx->size;

    while (written < out_len) {
        /* check EOF */
        if (ctx->pos + src_bps > data_end) break;

        /* read one source frame → int16 mono */
        int32_t mono = 0;
        if (ctx->bits == 8) {
            uint8_t L = ctx->data[ctx->pos];
            uint8_t R = (ctx->channels == 2) ? ctx->data[ctx->pos + 1] : L;
            mono = (int32_t)((L + R) >> 1); /* 0..255 range */
        } else {
            /* 16-bit signed little-endian */
            int16_t L = (int16_t)ru16(ctx->data + ctx->pos);
            int16_t R = (ctx->channels == 2)
                        ? (int16_t)ru16(ctx->data + ctx->pos + 2) : L;
            mono = (int32_t)((L + R) >> 1); /* -32768..32767 */
            /* convert to 0..255 unsigned */
            mono = (mono >> 8) + 128;
        }
        if (mono < 0) mono = 0;
        if (mono > 255) mono = 255;

        out[written++] = (uint8_t)mono;

        /* advance source pointer by step */
        frac_q16 += step_q16;
        while (frac_q16 >= 65536U) {
            frac_q16 -= 65536U;
            ctx->pos += src_bps;
            if (ctx->pos + src_bps > data_end) goto done;
        }
    }
done:
    return written;
}

void wav_seek(wav_ctx_t *ctx, uint32_t sample_pos)
{
    if (!ctx->data) return;
    uint32_t src_bps = (uint32_t)ctx->channels * (ctx->bits >> 3);
    /* sample_pos is in output (resampled) space; convert to source frame */
    uint32_t src_frame = (uint32_t)((uint64_t)sample_pos * ctx->sample_rate / AUDIO_OUT_RATE);
    ctx->pos = ctx->data_offset + src_frame * src_bps;
    uint32_t data_end = ctx->data_offset + ctx->data_size;
    if (ctx->pos > data_end) ctx->pos = data_end;
}

uint32_t wav_duration_sec(const wav_ctx_t *ctx)
{
    if (!ctx->sample_rate) return 0;
    return ctx->total_samples / ctx->sample_rate;
}

uint32_t wav_pos_sec(const wav_ctx_t *ctx)
{
    if (!ctx->data || !ctx->sample_rate) return 0;
    uint32_t src_bps = (uint32_t)ctx->channels * (ctx->bits >> 3);
    if (!src_bps) return 0;
    uint32_t src_frame = (ctx->pos - ctx->data_offset) / src_bps;
    return src_frame / ctx->sample_rate;
}
