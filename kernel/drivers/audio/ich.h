#ifndef ICH_H
#define ICH_H

#include <stdint.h>

/* ── Intel ICH AC97 (PCI class 04:01) ────────────────────────────────────────
   BAR0 = NAMBAR  (Native Audio Mixer, AC97 codec registers, I/O)
   BAR1 = NABMBAR (Native Audio Bus Master, DMA control, I/O)
   ─────────────────────────────────────────────────────────────────────────── */

/* NAMBAR (AC97 codec) register offsets – all 16-bit word access */
#define AC97_RESET          0x00
#define AC97_MASTER_VOL     0x02  /* 0x0000=max, bit15=mute, [14:8]=L att, [6:0]=R att */
#define AC97_HEADPHONE_VOL  0x04
#define AC97_MONO_VOL       0x06
#define AC97_PCM_OUT_VOL    0x18
#define AC97_EXT_AUDIO_ID   0x28
#define AC97_EXT_AUDIO_CTL  0x2A
#define AC97_FRONT_DAC_RATE 0x2C  /* PCM front DAC sample rate (VRA) */
#define AC97_EXT_AUDIO_VRA  0x0001

/* NABMBAR (bus master) PCM-out channel register offsets */
#define ICH_PCO_BDBAR   0x10  /* Buffer Descriptor List Base Address (32-bit) */
#define ICH_PCO_CIV     0x14  /* Current Index Value (byte, read-only) */
#define ICH_PCO_LVI     0x15  /* Last Valid Index (byte, writable) */
#define ICH_PCO_SR      0x16  /* Status Register (word) */
#define ICH_PCO_PICB    0x18  /* Position in Current Buffer (word, samples remaining) */
#define ICH_PCO_PIV     0x1A  /* Prefetched Index Value (byte) */
#define ICH_PCO_CR      0x1B  /* Control Register (byte) */

/* CR bits */
#define ICH_CR_RPBM     0x01  /* Run/Pause Bus Master */
#define ICH_CR_RR       0x02  /* Reset Registers (self-clearing) */
#define ICH_CR_LVBIE    0x04  /* Last Valid Buffer Interrupt Enable */
#define ICH_CR_FEIE     0x08  /* FIFO Error Interrupt Enable */
#define ICH_CR_IOCE     0x10  /* Interrupt On Completion Enable */

/* SR bits */
#define ICH_SR_DCH      0x01  /* DMA Controller Halted (done) */
#define ICH_SR_CELV     0x02  /* Current Equals Last Valid */
#define ICH_SR_LVBCI    0x04  /* Last Valid Buffer Completion Interrupt */
#define ICH_SR_BCIS     0x08  /* Buffer Completion Interrupt Status */
#define ICH_SR_FIFOE    0x10  /* FIFO Error */

/* Global registers (NABMBAR offsets) */
#define ICH_GLOB_CNT    0x2C  /* Global Control (32-bit) */
#define ICH_GLOB_STA    0x30  /* Global Status  (32-bit) */

/* GLOB_CNT bits (ICH4 / 82801 spec) */
#define ICH_GLOB_CNT_GIE     0x00000001  /* Global Interrupt Enable */
#define ICH_GLOB_CNT_COLD    0x00000002  /* Cold Reset: 0=assert, 1=deassert */
#define ICH_GLOB_CNT_WARM    0x00000004  /* Warm Reset (self-clearing) */
#define ICH_GLOB_CNT_ACLOFF  0x00000008  /* AC Link Off (0=active) */

/* GLOB_STA bits */
#define ICH_GLOB_STA_PCR     0x00000100  /* Primary Codec Ready (bit 8) */

/* BDL entry flags */
#define ICH_BDL_IOC     0x8000  /* Interrupt on Completion */
#define ICH_BDL_BUP     0x4000  /* Buffer Underrun Policy (play silence) */

/* Buffer Descriptor List entry – 8 bytes, packed */
typedef struct __attribute__((packed)) {
    uint32_t addr;      /* physical address of PCM buffer */
    uint16_t samples;   /* number of stereo sample-frames in this buffer */
    uint16_t flags;     /* ICH_BDL_IOC | ICH_BDL_BUP */
} ich_bdl_entry_t;

/*
 * ICH AC97 uses a 32-entry circular BDL ring (VirtualBox/QEMU always use % 32).
 * We keep 4 actual PCM buffers (rotating: BDL[i] → pcm_buf[i%4]).
 * Memory: 32×8 = 256 bytes BDL + 4×36864 = 147456 bytes PCM = ~144 KB total.
 *
 * Resampled to 48000 Hz stereo 16-bit from 22050 Hz 8-bit mono:
 *   out_frames = 4096 * 48000 / 22050 ≈ 8921  → 9216 (safe margin)
 */
#define ICH_BDL_ENTRIES   32   /* full ring — VirtualBox requires this */
#define ICH_BUF_SLOTS      4   /* actual PCM buffer count (rotated) */
#define ICH_BUF_SAMPLES   9216
#define ICH_BUF_BYTES     (ICH_BUF_SAMPLES * 4)

typedef struct {
    int      present;
    uint32_t nambar;
    uint32_t nabmbar;
    int      has_vra;
    uint32_t rate;
    uint8_t  volume;

    ich_bdl_entry_t bdl[ICH_BDL_ENTRIES] __attribute__((aligned(8)));
    int16_t  pcm_buf[ICH_BUF_SLOTS][ICH_BUF_SAMPLES * 2]; /* L,R interleaved */
    int      next_lvi;  /* next BDL entry index to fill (advances mod 32) */
} ich_dev_t;

int  ich_audio_init(ich_dev_t *dev);
void ich_audio_set_volume(ich_dev_t *dev, uint8_t vol_0_100);
int  ich_audio_submit(ich_dev_t *dev, const uint8_t *pcm8_mono22, uint32_t n);
int  ich_audio_busy(ich_dev_t *dev);
void ich_audio_stop(ich_dev_t *dev);

#endif
