#ifndef ICH_H
#define ICH_H

#include <stdint.h>

/* ── ICH AC97 / Intel HDA-AC97 (PCI class 04:01) ─────────────────────────
   Tested: VirtualBox ICH AC97, QEMU -device AC97
   BAR0 = NAMBAR  (Native Audio Mixer, AC97 codec registers, I/O)
   BAR1 = NABMBAR (Native Audio Bus Master, DMA control, I/O)
   ─────────────────────────────────────────────────────────────────────── */

/* NAMBAR (codec) register offsets */
#define AC97_RESET          0x00
#define AC97_MASTER_VOL     0x02  /* 0x0000 = max, bit15 = mute */
#define AC97_HEADPHONE_VOL  0x04
#define AC97_MONO_VOL       0x06
#define AC97_PC_BEEP        0x0A
#define AC97_PCM_OUT_VOL    0x18  /* PCM output volume */
#define AC97_EXT_AUDIO_ID   0x28  /* Extended Audio ID */
#define AC97_EXT_AUDIO_CTL  0x2A  /* Extended Audio Status/Control */
#define AC97_FRONT_DAC_RATE 0x2C  /* PCM output sample rate (VRA) */
#define AC97_EXT_AUDIO_VRA  0x0001

/* NABMBAR (bus master) register offsets — PCM Out channel */
#define ICH_PCO_BDBAR   0x10  /* Buffer Descriptor List Base Address */
#define ICH_PCO_CIV     0x14  /* Current Index Value (byte) */
#define ICH_PCO_LVI     0x15  /* Last Valid Index (byte) */
#define ICH_PCO_SR      0x16  /* Status (word): bit0=halt, bit1=EOL, bit2=LVBCI */
#define ICH_PCO_PICB    0x18  /* Position in Current Buffer (word, samples left) */
#define ICH_PCO_PIV     0x1A  /* Prefetched Index Value (byte) */
#define ICH_PCO_CR      0x1B  /* Control (byte) */

#define ICH_CR_RPBM     0x01  /* Run/Pause Bus Master */
#define ICH_CR_RR       0x02  /* Reset Registers */
#define ICH_CR_LVBIE    0x04  /* Last Valid Buffer Interrupt Enable */
#define ICH_CR_FEIE     0x08  /* FIFO Error Interrupt Enable */
#define ICH_CR_IOCE     0x10  /* Interrupt On Completion Enable */

#define ICH_SR_DCH      0x01  /* DMA Controller Halted */
#define ICH_SR_CELV     0x02  /* Current Equals Last Valid */
#define ICH_SR_LVBCI    0x04  /* Last Valid Buffer Completion Interrupt */
#define ICH_SR_BCIS     0x08  /* Buffer Completion Interrupt Status */
#define ICH_SR_FIFOE    0x10  /* FIFO Error */

/* Global Status (NABMBAR + 0x30) */
#define ICH_GLOB_STA    0x30
#define ICH_GLOB_CNT    0x2C
#define ICH_GLOB_CNT_ACLOFF  0x00000008  /* AC Link Off */
#define ICH_GLOB_CNT_WARM    0x00000002  /* Warm Reset */
#define ICH_GLOB_CNT_COLD    0x00000001  /* Cold Reset */

/* BDL entry flags */
#define ICH_BDL_IOC     0x8000  /* Interrupt on Completion */
#define ICH_BDL_BUP     0x4000  /* Buffer Underrun Policy (silence) */

/* Buffer Descriptor List entry (8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t addr;      /* physical address of audio buffer */
    uint16_t samples;   /* number of 16-bit stereo samples (pairs) */
    uint16_t flags;     /* ICH_BDL_IOC | ICH_BDL_BUP */
} ich_bdl_entry_t;

#define ICH_BDL_ENTRIES   4       /* ping-pong with 4 slots */
#define ICH_BUF_SAMPLES   2048    /* 16-bit stereo samples per slot */
#define ICH_BUF_BYTES     (ICH_BUF_SAMPLES * 4)  /* 4 bytes per stereo sample */

typedef struct {
    int      present;
    uint32_t nambar;    /* NAMBAR I/O base */
    uint32_t nabmbar;   /* NABMBAR I/O base */
    int      has_vra;   /* Variable Rate Audio support */
    uint32_t rate;      /* actual output rate */
    uint8_t  volume;    /* 0-100 */

    /* DMA buffers – must be below 4 GB (trivially true in 32-bit kernel) */
    ich_bdl_entry_t bdl[ICH_BDL_ENTRIES] __attribute__((aligned(8)));
    /* 4 PCM output buffers (16-bit stereo 48 kHz) */
    int16_t  pcm_buf[ICH_BDL_ENTRIES][ICH_BUF_SAMPLES * 2];
    int      next_buf;  /* next slot to fill */
} ich_dev_t;

int  ich_audio_init(ich_dev_t *dev);
void ich_audio_set_volume(ich_dev_t *dev, uint8_t vol_0_100);

/* Submit up to ICH_BUF_SAMPLES 8-bit unsigned mono samples at 22050 Hz.
   Converts internally to 16-bit signed stereo at 48000 Hz.
   Returns 0 on success, -1 if busy / unavailable. */
int  ich_audio_submit(ich_dev_t *dev, const uint8_t *pcm8_mono22, uint32_t n);

int  ich_audio_busy(ich_dev_t *dev);
void ich_audio_stop(ich_dev_t *dev);

#endif
