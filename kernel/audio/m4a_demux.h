#ifndef M4A_DEMUX_H
#define M4A_DEMUX_H

#include <stdint.h>

/* Minimal ISO-BMFF (MP4/M4A) demuxer: walks moov/trak/mdia/minf/stbl to
 * find the first AAC audio track, reads its AudioSpecificConfig out of
 * esds, and re-packages every raw (ADTS-less) AAC-LC sample stored in
 * mdat as a standard ADTS stream — the existing aac_decoder.c only
 * understands ADTS framing, and M4A/MP4 files never include it (frame
 * boundaries instead come from the stsz/stsc/stco sample tables), so
 * this is the bridge between the two. */

typedef struct {
    uint8_t *adts_buf; /* malloc'd — caller must free via m4a_free() */
    uint32_t adts_len;
    uint32_t sample_rate;
    uint8_t  channels;
} m4a_result_t;

/* Returns 0 on success (adts_buf/adts_len ready to hand to aac_open()),
 * -1 if this isn't a parseable M4A/MP4-AAC file. */
int  m4a_demux(const uint8_t *data, uint32_t size, m4a_result_t *out);
void m4a_free(m4a_result_t *out);

#endif
