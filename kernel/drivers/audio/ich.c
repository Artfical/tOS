/*
 * ich.c  –  Intel ICH AC97 audio output driver
 *
 * Supports:
 *   VirtualBox  : Intel ICH AC97  (PCI 8086:2415 / 8086:2445)
 *   QEMU        : -device AC97   (PCI 8086:2415)
 *   Real HW     : ICH, ICH2, ICH4 southbridges
 *
 * Output path:
 *   8-bit unsigned mono 22050 Hz (from audio.c)
 *     → upsample to 48000 Hz (nearest-neighbour)
 *     → convert to signed 16-bit
 *     → duplicate to stereo
 *     → write to 16-bit stereo PCM DMA buffer
 *     → ICH AC97 plays via BDL (Buffer Descriptor List) DMA
 */

#include "ich.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "klog.h"

/* ── PCI IDs for ICH AC97 ─────────────────────────────────────────────── */
#define ICH_VENDOR  0x8086
static const uint16_t ich_devids[] = {
    0x2415, /* ICH  AC97 */
    0x2425, /* ICH0 AC97 */
    0x2445, /* ICH2 AC97 */
    0x2485, /* ICH3 AC97 */
    0x24C5, /* ICH4 AC97 */
    0x24D5, /* ICH5 AC97 */
    0x25A6, /* ESB  AC97 */
    0x266E, /* ICH6 AC97 */
    0x27DE, /* ICH7 AC97 */
    0x7195, /* 440MX AC97 */
    0
};

/* ── NAMBAR helpers (AC97 codec, 16-bit I/O) ─────────────────────────── */
static uint16_t nam_read(uint32_t base, uint8_t reg)
{
    return inw((uint16_t)(base + reg));
}
static void nam_write(uint32_t base, uint8_t reg, uint16_t val)
{
    outw((uint16_t)(base + reg), val);
}

/* ── NABMBAR helpers (bus-master, 8/16/32-bit) ───────────────────────── */
static uint8_t  bm_read8 (uint32_t base, uint8_t off) { return inb ((uint16_t)(base+off)); }
static uint16_t bm_read16(uint32_t base, uint8_t off) __attribute__((unused));
static uint16_t bm_read16(uint32_t base, uint8_t off) { return inw ((uint16_t)(base+off)); }
static void bm_write8 (uint32_t base, uint8_t off, uint8_t  v) { outb((uint16_t)(base+off), v); }
static void bm_write16(uint32_t base, uint8_t off, uint16_t v) { outw((uint16_t)(base+off), v); }
static void bm_write32(uint32_t base, uint8_t off, uint32_t v) { outl((uint16_t)(base+off), v); }

/* ── Busy-wait for codec ready ──────────────────────────────────────── */
static int codec_ready(uint32_t nabmbar)
{
    for (int i = 0; i < 100000; i++) {
        uint32_t sta = inl((uint16_t)(nabmbar + ICH_GLOB_STA));
        if (sta & (1 << 8)) return 0; /* Primary codec ready */
    }
    return -1; /* timeout */
}

/* ── PCI enable Bus Master + I/O ────────────────────────────────────── */
static void pci_enable_bm(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t cmd = pci_read_config(bus, dev, fn, 0x04);
    cmd |= 0x05; /* I/O space + Bus Master */
    pci_write_config(bus, dev, fn, 0x04, cmd);
}

/* ── Init ─────────────────────────────────────────────────────────────── */
int ich_audio_init(ich_dev_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->present = 0;

    /* Find ICH AC97 on PCI bus */
    pci_device_t pdevs[4];
    /* class 04 (multimedia), subclass 01 (audio) */
    int n = pci_find_devices(0x04, 0x01, pdevs, 4);
    if (!n) {
        klog_write("ich_ac97: no PCI multimedia audio device found\n");
        return -1;
    }

    /* Check vendor + device ID for known ICH AC97 */
    pci_device_t *pd = NULL;
    for (int i = 0; i < n; i++) {
        if (pdevs[i].vendor_id != ICH_VENDOR) continue;
        for (int j = 0; ich_devids[j]; j++) {
            if (pdevs[i].device_id == ich_devids[j]) {
                pd = &pdevs[i]; break;
            }
        }
        if (pd) break;
    }
    if (!pd) {
        klog_write("ich_ac97: no ICH AC97 device found\n");
        return -1;
    }

    pci_enable_bm(pd->bus, pd->device, pd->func);

    dev->nambar  = pci_get_bar(pd->bus, pd->device, pd->func, 0) & 0xFFFFFFFEU;
    dev->nabmbar = pci_get_bar(pd->bus, pd->device, pd->func, 1) & 0xFFFFFFFEU;
    if (!dev->nambar || !dev->nabmbar) {
        klog_write("ich_ac97: invalid BARs\n");
        return -1;
    }

    /* Cold reset AC link */
    uint32_t gcnt = inl((uint16_t)(dev->nabmbar + ICH_GLOB_CNT));
    gcnt &= ~ICH_GLOB_CNT_ACLOFF;
    gcnt |=  ICH_GLOB_CNT_COLD;
    outl((uint16_t)(dev->nabmbar + ICH_GLOB_CNT), gcnt);
    for (volatile int i = 0; i < 200000; i++);

    /* Wait for codec ready */
    if (codec_ready(dev->nabmbar) != 0) {
        klog_write("ich_ac97: codec not ready after reset\n");
        return -1;
    }

    /* Reset codec */
    nam_write(dev->nambar, AC97_RESET, 0);
    for (volatile int i = 0; i < 50000; i++);

    /* Volume: master 0dB, PCM out 0dB */
    nam_write(dev->nambar, AC97_MASTER_VOL,  0x0000); /* max */
    nam_write(dev->nambar, AC97_HEADPHONE_VOL, 0x0000);
    nam_write(dev->nambar, AC97_PCM_OUT_VOL, 0x0000); /* max */

    /* Check VRA (Variable Rate Audio) support */
    uint16_t ext_id = nam_read(dev->nambar, AC97_EXT_AUDIO_ID);
    if (ext_id & AC97_EXT_AUDIO_VRA) {
        /* Enable VRA */
        uint16_t ctl = nam_read(dev->nambar, AC97_EXT_AUDIO_CTL);
        nam_write(dev->nambar, AC97_EXT_AUDIO_CTL, ctl | AC97_EXT_AUDIO_VRA);
        /* Set 48000 Hz (safest, all ICH AC97 support it) */
        nam_write(dev->nambar, AC97_FRONT_DAC_RATE, 48000);
        dev->has_vra = 1;
        dev->rate = 48000;
    } else {
        /* No VRA: codec outputs at 48000 Hz always */
        dev->rate = 48000;
    }

    /* Stop PCM out DMA channel */
    bm_write8(dev->nabmbar, ICH_PCO_CR, 0);
    bm_write8(dev->nabmbar, ICH_PCO_CR, ICH_CR_RR); /* reset */
    for (volatile int i = 0; i < 10000; i++);
    bm_write8(dev->nabmbar, ICH_PCO_CR, 0);

    /* Build Buffer Descriptor List */
    for (int i = 0; i < ICH_BDL_ENTRIES; i++) {
        memset(dev->pcm_buf[i], 0, ICH_BUF_BYTES);
        dev->bdl[i].addr    = (uint32_t)(unsigned long)dev->pcm_buf[i];
        dev->bdl[i].samples = ICH_BUF_SAMPLES;
        dev->bdl[i].flags   = ICH_BDL_IOC | ICH_BDL_BUP;
    }

    /* Point BDBAR at our BDL */
    bm_write32(dev->nabmbar, ICH_PCO_BDBAR, (uint32_t)(unsigned long)dev->bdl);
    /* LVI = last valid = ICH_BDL_ENTRIES - 1 (circular) */
    bm_write8(dev->nabmbar, ICH_PCO_LVI, (uint8_t)(ICH_BDL_ENTRIES - 1));

    /* Start DMA */
    bm_write8(dev->nabmbar, ICH_PCO_CR, ICH_CR_RPBM);

    dev->present  = 1;
    dev->volume   = 80;
    dev->next_buf = 0;

    klog_write("ich_ac97: init OK, 48000 Hz 16-bit stereo DMA\n");
    return 0;
}

/* ── Volume ──────────────────────────────────────────────────────────── */
void ich_audio_set_volume(ich_dev_t *dev, uint8_t vol)
{
    if (!dev->present) return;
    dev->volume = vol;
    /* AC97 master volume: 6-bit attenuation per channel (0=0dB, 63=-94.5dB)
       bits[14:8]=left, bits[6:0]=right, bit15=mute */
    uint8_t att = (uint8_t)((100 - vol) * 63 / 100);
    uint16_t reg = (uint16_t)(((uint16_t)att << 8) | att);
    if (vol == 0) reg |= 0x8000; /* mute */
    nam_write(dev->nambar, AC97_MASTER_VOL, reg);
    nam_write(dev->nambar, AC97_PCM_OUT_VOL, reg);
}

/* ── Busy check ─────────────────────────────────────────────────────── */
int ich_audio_busy(ich_dev_t *dev)
{
    if (!dev->present) return 0;
    /* Check if current slot is still playing */
    uint8_t civ = bm_read8(dev->nabmbar, ICH_PCO_CIV);
    /* If CIV reached our next_buf index, the slot is free */
    return (civ == (uint8_t)((dev->next_buf) % ICH_BDL_ENTRIES)) ? 0 : 1;
}

/* ── Stop ────────────────────────────────────────────────────────────── */
void ich_audio_stop(ich_dev_t *dev)
{
    if (!dev->present) return;
    bm_write8(dev->nabmbar, ICH_PCO_CR, 0);     /* pause DMA */
    bm_write16(dev->nabmbar, ICH_PCO_SR, 0x1C); /* clear status bits */
    /* clear all PCM buffers */
    for (int i = 0; i < ICH_BDL_ENTRIES; i++)
        memset(dev->pcm_buf[i], 0, ICH_BUF_BYTES);
    dev->next_buf = 0;
    /* restart DMA (keep running but with silence) */
    bm_write8(dev->nabmbar, ICH_PCO_CR, ICH_CR_RPBM);
}

/* ── Submit (8-bit mono 22050 Hz → 16-bit stereo 48000 Hz) ──────────── */
int ich_audio_submit(ich_dev_t *dev, const uint8_t *pcm8, uint32_t n8)
{
    if (!dev->present) return -1;

    /* Which BDL slot to fill next? */
    int slot = dev->next_buf % ICH_BDL_ENTRIES;
    int16_t *dst = dev->pcm_buf[slot];

    /* Resample: 22050 → 48000 (step = 22050/48000 in Q16) */
    uint32_t step = (uint32_t)((uint64_t)22050 * 65536 / 48000); /* ≈ 30109 */
    uint32_t frac = 0;
    uint32_t src  = 0;
    int      out  = 0;

    while (out < ICH_BUF_SAMPLES && src < n8) {
        /* 8-bit unsigned → signed 16-bit */
        int16_t s = (int16_t)(((int32_t)pcm8[src] - 128) << 8);
        dst[out * 2]     = s; /* L */
        dst[out * 2 + 1] = s; /* R */
        out++;
        frac += step;
        while (frac >= 65536U) {
            frac -= 65536U;
            src++;
        }
    }
    /* pad remainder with silence */
    while (out < ICH_BUF_SAMPLES) {
        dst[out * 2] = dst[out * 2 + 1] = 0;
        out++;
    }

    /* advance LVI so this slot becomes "valid" */
    dev->next_buf = (dev->next_buf + 1) % ICH_BDL_ENTRIES;
    bm_write8(dev->nabmbar, ICH_PCO_LVI,
              (uint8_t)((dev->next_buf + ICH_BDL_ENTRIES - 1) % ICH_BDL_ENTRIES));

    /* make sure DMA is running */
    uint8_t cr = bm_read8(dev->nabmbar, ICH_PCO_CR);
    if (!(cr & ICH_CR_RPBM))
        bm_write8(dev->nabmbar, ICH_PCO_CR, ICH_CR_RPBM);

    return 0;
}
