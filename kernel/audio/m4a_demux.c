#include "m4a_demux.h"
#include "memory.h"
#include "string.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint32_t)p[0]<<8)|p[1]); }

/* Finds a direct child box by fourcc within [base, base+size). Returns a
 * pointer to the box (at its 4-byte size field) and its total size
 * (including the 8-byte header), or NULL if not present at this level. */
static const uint8_t *find_box(const uint8_t *base, uint32_t size, const char *fourcc, uint32_t *out_size)
{
    uint32_t pos = 0;
    while (pos + 8 <= size) {
        uint32_t bsize = be32(base + pos);
        if (bsize < 8 || pos + bsize > size) break;
        if (memcmp(base + pos + 4, fourcc, 4) == 0) {
            *out_size = bsize;
            return base + pos;
        }
        pos += bsize;
    }
    return NULL;
}

/* MPEG-4 descriptor tag/length parsing (ISO/IEC 14496-1), used to dig
 * AudioSpecificConfig (tag 0x05) out of esds — recurses into every
 * descriptor's payload since the target is nested two levels deep
 * (ES_Descriptor[0x03] -> DecoderConfigDescriptor[0x04] ->
 * DecoderSpecificInfo[0x05]) and this is simpler than modeling the
 * exact container hierarchy. */
static const uint8_t *find_desc_tag(const uint8_t *data, uint32_t len, uint8_t tag, uint32_t *out_len)
{
    const uint8_t *p = data, *end = data + len;
    while (p + 2 <= end) {
        uint8_t t = *p++;
        uint32_t sz = 0;
        int i = 0;
        while (p < end && i < 4) {
            uint8_t b = *p++;
            sz = (sz << 7) | (uint32_t)(b & 0x7F);
            i++;
            if (!(b & 0x80)) break;
        }
        if (p + sz > end) return NULL;
        if (t == tag) { *out_len = sz; return p; }
        const uint8_t *found = find_desc_tag(p, sz, tag, out_len);
        if (found) return found;
        p += sz;
    }
    return NULL;
}

static const uint32_t ADTS_FREQ_TABLE[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
};

static uint32_t freq_to_index(uint32_t rate)
{
    for (int i = 0; i < 13; i++) if (ADTS_FREQ_TABLE[i] == rate) return (uint32_t)i;
    return 4; /* default to 44100 if unrecognised */
}

int m4a_demux(const uint8_t *data, uint32_t size, m4a_result_t *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t moov_sz;
    const uint8_t *moov = find_box(data, size, "moov", &moov_sz);
    if (!moov) return -1;

    uint32_t trak_sz;
    const uint8_t *trak = find_box(moov + 8, moov_sz - 8, "trak", &trak_sz);
    if (!trak) return -1;

    uint32_t mdia_sz;
    const uint8_t *mdia = find_box(trak + 8, trak_sz - 8, "mdia", &mdia_sz);
    if (!mdia) return -1;

    uint32_t minf_sz;
    const uint8_t *minf = find_box(mdia + 8, mdia_sz - 8, "minf", &minf_sz);
    if (!minf) return -1;

    uint32_t stbl_sz;
    const uint8_t *stbl = find_box(minf + 8, minf_sz - 8, "stbl", &stbl_sz);
    if (!stbl) return -1;

    uint32_t stsd_sz;
    const uint8_t *stsd = find_box(stbl + 8, stbl_sz - 8, "stsd", &stsd_sz);
    if (!stsd || stsd_sz < 16) return -1;

    /* stsd: fullbox header(4) + entry_count(4), then the first entry
     * (an 'mp4a' SampleEntry/AudioSampleEntry) starts right after. */
    const uint8_t *mp4a = stsd + 16;
    if (stsd + stsd_sz < mp4a + 8 || memcmp(mp4a + 4, "mp4a", 4) != 0) return -1;
    uint32_t mp4a_sz = be32(mp4a);
    if (mp4a + mp4a_sz > stsd + stsd_sz) return -1;

    /* AudioSampleEntry fixed fields (28 bytes) precede any child boxes. */
    const uint8_t *ase_body = mp4a + 8;
    uint32_t ase_len = mp4a_sz - 8;
    if (ase_len < 28) return -1;
    uint32_t default_channels = be16(ase_body + 8);
    uint32_t default_rate = be32(ase_body + 24) >> 16;

    uint32_t esds_sz;
    const uint8_t *esds = find_box(ase_body + 28, ase_len - 28, "esds", &esds_sz);

    uint32_t sample_rate = default_rate ? default_rate : 44100;
    uint32_t channels = default_channels ? default_channels : 2;
    uint32_t aot = 2; /* assume AAC-LC unless AudioSpecificConfig says otherwise */

    if (esds && esds_sz > 12) {
        uint32_t asc_len = 0;
        const uint8_t *asc = find_desc_tag(esds + 12, esds_sz - 12, 0x05, &asc_len);
        if (asc && asc_len >= 2) {
            aot = (asc[0] >> 3) & 0x1F;
            uint32_t sfi = (uint32_t)((asc[0] & 0x07) << 1) | (asc[1] >> 7);
            uint32_t cc = (asc[1] >> 3) & 0x0F;
            if (sfi < 13) sample_rate = ADTS_FREQ_TABLE[sfi];
            if (cc > 0) channels = cc;
        }
    }
    if (aot != 2) return -1; /* only plain AAC-LC (no SBR/PS/HE-AAC) is supported */

    uint32_t stsz_sz, stsc_sz, stco_sz;
    const uint8_t *stsz = find_box(stbl + 8, stbl_sz - 8, "stsz", &stsz_sz);
    const uint8_t *stsc = find_box(stbl + 8, stbl_sz - 8, "stsc", &stsc_sz);
    const uint8_t *stco = find_box(stbl + 8, stbl_sz - 8, "stco", &stco_sz);
    int co64_mode = 0;
    if (!stco) { stco = find_box(stbl + 8, stbl_sz - 8, "co64", &stco_sz); co64_mode = 1; }
    if (!stsz || !stsc || !stco || stsz_sz < 20) return -1;

    uint32_t default_size = be32(stsz + 12);
    uint32_t sample_count = be32(stsz + 16);
    const uint8_t *stsz_sizes = stsz + 20;
    if (sample_count == 0 || sample_count > 500000) return -1;
    if (default_size == 0 && stsz + stsz_sz < stsz_sizes + (uint32_t)sample_count * 4) return -1;

    uint32_t stsc_count = be32(stsc + 12);
    const uint8_t *stsc_ent = stsc + 16;
    if (stsc_count == 0 || stsc_count > 100000 || stsc + stsc_sz < stsc_ent + stsc_count * 12) return -1;

    uint32_t chunk_count = be32(stco + 12);
    const uint8_t *stco_ent = stco + 16;
    uint32_t chunk_entry_sz = co64_mode ? 8 : 4;
    if (chunk_count == 0 || chunk_count > 200000 ||
        stco + stco_sz < stco_ent + chunk_count * chunk_entry_sz) return -1;

    /* Total output size is exact and known up front (sum of sample
     * sizes + a 7-byte ADTS header per sample), so a single malloc
     * suffices — no growable-buffer/realloc bookkeeping needed. */
    uint64_t total_payload = 0;
    if (default_size != 0) {
        total_payload = (uint64_t)default_size * sample_count;
    } else {
        for (uint32_t i = 0; i < sample_count; i++) total_payload += be32(stsz_sizes + i * 4);
    }
    uint64_t total_out = total_payload + (uint64_t)sample_count * 7U;
    if (total_out == 0 || total_out > 128U * 1024U * 1024U) return -1;

    uint8_t *outbuf = (uint8_t *)malloc((uint32_t)total_out);
    if (!outbuf) return -1;

    uint32_t freq_idx = freq_to_index(sample_rate);
    uint32_t sample_idx = 0, out_off = 0;

    for (uint32_t si = 0; si < stsc_count && sample_idx < sample_count; si++) {
        uint32_t first_chunk = be32(stsc_ent + si * 12 + 0);
        uint32_t spc = be32(stsc_ent + si * 12 + 4);
        uint32_t last_chunk = (si + 1 < stsc_count) ? be32(stsc_ent + (si + 1) * 12 + 0) - 1 : chunk_count;

        for (uint32_t chunk = first_chunk; chunk <= last_chunk && chunk <= chunk_count && sample_idx < sample_count; chunk++) {
            uint64_t offset = co64_mode
                ? (((uint64_t)be32(stco_ent + (chunk - 1) * 8) << 32) | be32(stco_ent + (chunk - 1) * 8 + 4))
                : be32(stco_ent + (chunk - 1) * 4);

            for (uint32_t s = 0; s < spc && sample_idx < sample_count; s++) {
                uint32_t samp_size = default_size ? default_size : be32(stsz_sizes + sample_idx * 4);
                if (offset + samp_size > size || out_off + 7 + samp_size > total_out) { free(outbuf); return -1; }

                uint8_t *h = outbuf + out_off;
                h[0] = 0xFF;
                h[1] = 0xF1; /* MPEG-4, no CRC */
                h[2] = (uint8_t)(((aot - 1) << 6) | (freq_idx << 2) | ((channels >> 2) & 0x01));
                uint32_t frame_len = 7 + samp_size;
                h[3] = (uint8_t)(((channels & 0x03) << 6) | ((frame_len >> 11) & 0x03));
                h[4] = (uint8_t)((frame_len >> 3) & 0xFF);
                h[5] = (uint8_t)(((frame_len & 0x07) << 5) | 0x1F);
                h[6] = 0xFC;

                memcpy(outbuf + out_off + 7, data + offset, samp_size);
                out_off += 7 + samp_size;
                offset += samp_size;
                sample_idx++;
            }
        }
    }

    out->adts_buf = outbuf;
    out->adts_len = out_off;
    out->sample_rate = sample_rate;
    out->channels = (uint8_t)channels;
    return 0;
}

void m4a_free(m4a_result_t *out)
{
    if (out->adts_buf) { free(out->adts_buf); out->adts_buf = NULL; }
    out->adts_len = 0;
}
