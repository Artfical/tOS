/*
 * ich.c – Intel ICH AC97 audio output driver
 *
 * Supports VirtualBox ICH AC97, QEMU -device AC97, real ICH southbridges.
 *
 * Key fix over original: GLOB_CNT bit 1 is the Cold Reset deassert bit,
 * NOT bit 0 (which is GIE). Wrong bit was causing codec to never wake up.
 */

#include "ich.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "klog.h"

#define ICH_VENDOR  0x8086
static const uint16_t ich_devids[] = {
    0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5,
    0x25A6, 0x266E, 0x27DE, 0x7195, 0
};

/* ── I/O helpers ─────────────────────────────────────────────────────────── */
static uint8_t  bm8 (uint32_t b, uint8_t o) { return inb ((uint16_t)(b+o)); }
static void     bm8w(uint32_t b, uint8_t o, uint8_t  v){ outb((uint16_t)(b+o),v); }
static void     bm16w(uint32_t b,uint8_t o, uint16_t v){ outw((uint16_t)(b+o),v); }
static uint32_t bm32r(uint32_t b, uint8_t o){ return inl ((uint16_t)(b+o)); }
static void     bm32w(uint32_t b, uint8_t o, uint32_t v){ outl((uint16_t)(b+o),v); }

static uint16_t nam_r(uint32_t b, uint8_t r){ return inw((uint16_t)(b+r)); }
static void     nam_w(uint32_t b, uint8_t r, uint16_t v){ outw((uint16_t)(b+r),v); }

static void delay_us(int n)
{
    /* rough busy-wait: ~1 µs per iteration at ~1 GHz; n = microseconds */
    for (volatile int i = 0; i < n * 100; i++);
}

/* ── PCI bus master enable ───────────────────────────────────────────────── */
static void pci_bm_enable(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t cmd = pci_read_config(bus, dev, fn, 0x04);
    cmd |= 0x05; /* I/O space + Bus Master */
    pci_write_config(bus, dev, fn, 0x04, cmd);
}

/* ── Init ────────────────────────────────────────────────────────────────── */
int ich_audio_init(ich_dev_t *dev)
{
    memset(dev, 0, sizeof(*dev));

    /* Find ICH AC97 on PCI */
    pci_device_t pdevs[4];
    int n = pci_find_devices(0x04, 0x01, pdevs, 4);
    if (!n) { klog_write("ich_ac97: no class 04:01 device\n"); return -1; }

    pci_device_t *pd = NULL;
    for (int i = 0; i < n && !pd; i++) {
        if (pdevs[i].vendor_id != ICH_VENDOR) continue;
        for (int j = 0; ich_devids[j] && !pd; j++)
            if (pdevs[i].device_id == ich_devids[j]) pd = &pdevs[i];
    }
    if (!pd) {
        /* Only Intel ICH's AC97 register layout (NAM/NABM at BAR0/1) is
         * supported here -- other class 04:01 audio controllers (e.g.
         * the Ensoniq ES1371 some hypervisors, including VMware,
         * default to) use a completely different programming interface
         * and would need their own driver. Log every non-matching
         * card's actual vendor/device ID so a future driver can target
         * the right chip instead of guessing. */
        for (int i = 0; i < n; i++) {
            char line[64]; int lp = 0;
            const char *p = "ich_ac97: unsupported audio dev vendor=0x";
            while (*p) line[lp++] = *p++;
            for (int k = 12; k >= 0; k -= 4) line[lp++] = "0123456789ABCDEF"[(pdevs[i].vendor_id >> k) & 0xF];
            p = " device=0x";
            while (*p) line[lp++] = *p++;
            for (int k = 12; k >= 0; k -= 4) line[lp++] = "0123456789ABCDEF"[(pdevs[i].device_id >> k) & 0xF];
            line[lp++] = '\n';
            line[lp] = '\0';
            klog_write(line);
        }
        klog_write("ich_ac97: no Intel AC97 device ID match\n");
        return -1;
    }

    pci_bm_enable(pd->bus, pd->device, pd->func);

    uint32_t nambar  = pci_get_bar(pd->bus, pd->device, pd->func, 0) & ~3U;
    uint32_t nabmbar = pci_get_bar(pd->bus, pd->device, pd->func, 1) & ~3U;
    if (!nambar || !nabmbar) { klog_write("ich_ac97: bad BARs\n"); return -1; }
    dev->nambar  = nambar;
    dev->nabmbar = nabmbar;

    /* ── AC link cold reset ──────────────────────────────────────────────── *
     * GLOB_CNT bit 1 = Cold Reset deassert (0=assert/reset, 1=normal).
     * Step 1: assert reset (bit1=0), Step 2: deassert (bit1=1), wait ready. */
    uint32_t gcnt = bm32r(nabmbar, ICH_GLOB_CNT);
    gcnt &= ~(ICH_GLOB_CNT_COLD | ICH_GLOB_CNT_WARM | ICH_GLOB_CNT_ACLOFF);
    gcnt &= ~ICH_GLOB_CNT_GIE;          /* no interrupts */
    bm32w(nabmbar, ICH_GLOB_CNT, gcnt); /* assert cold reset */
    delay_us(10000);                     /* 10 ms */

    gcnt |= ICH_GLOB_CNT_COLD;          /* deassert cold reset → normal */
    bm32w(nabmbar, ICH_GLOB_CNT, gcnt);
    delay_us(50000);                     /* 50 ms: codec needs time to wake */

    /* Poll GLOB_STA bit 8 (Primary Codec Ready) up to ~500 ms */
    int ready = 0;
    for (int i = 0; i < 500 && !ready; i++) {
        if (bm32r(nabmbar, ICH_GLOB_STA) & ICH_GLOB_STA_PCR) ready = 1;
        else delay_us(1000);
    }
    if (!ready) {
        /* Some implementations don't set bit 8; try probing codec anyway */
        klog_write("ich_ac97: codec PCR timeout, continuing anyway\n");
    }

    /* ── Codec (AC97 mixer) reset and volume ────────────────────────────── */
    nam_w(nambar, AC97_RESET, 0);        /* reset AC97 codec */
    delay_us(10000);

    /* check codec responds (if all 0xFFFF → not ready) */
    uint16_t master = nam_r(nambar, AC97_MASTER_VOL);
    if (master == 0xFFFF) {
        klog_write("ich_ac97: codec not responding (0xFFFF)\n");
        return -1;
    }

    nam_w(nambar, AC97_MASTER_VOL,    0x0000); /* max volume, no mute */
    nam_w(nambar, AC97_HEADPHONE_VOL, 0x0000);
    nam_w(nambar, AC97_PCM_OUT_VOL,   0x0000);

    /* ── VRA (Variable Rate Audio) ──────────────────────────────────────── */
    uint16_t ext_id = nam_r(nambar, AC97_EXT_AUDIO_ID);
    if (ext_id & AC97_EXT_AUDIO_VRA) {
        uint16_t ctl = nam_r(nambar, AC97_EXT_AUDIO_CTL);
        nam_w(nambar, AC97_EXT_AUDIO_CTL, ctl | AC97_EXT_AUDIO_VRA);
        nam_w(nambar, AC97_FRONT_DAC_RATE, 48000);
        dev->has_vra = 1;
    }
    dev->rate = 48000;

    /* ── Reset PCM-out DMA channel ──────────────────────────────────────── */
    bm8w(nabmbar, ICH_PCO_CR, 0);
    bm8w(nabmbar, ICH_PCO_CR, ICH_CR_RR);
    delay_us(10000);
    bm8w(nabmbar, ICH_PCO_CR, 0);
    bm16w(nabmbar, ICH_PCO_SR, 0x1C);   /* clear status bits */

    /* ── Build BDL (32 entries, rotating through ICH_BUF_SLOTS PCM buffers) ── */
    for (int i = 0; i < ICH_BUF_SLOTS; i++)
        memset(dev->pcm_buf[i], 0, ICH_BUF_BYTES);
    for (int i = 0; i < ICH_BDL_ENTRIES; i++) {
        dev->bdl[i].addr    = (uint32_t)(unsigned long)dev->pcm_buf[i % ICH_BUF_SLOTS];
        dev->bdl[i].samples = ICH_BUF_SAMPLES;
        dev->bdl[i].flags   = ICH_BDL_BUP;
    }

    /* Point BDBAR at BDL; LVI=0; do NOT start DMA yet */
    bm32w(nabmbar, ICH_PCO_BDBAR, (uint32_t)(unsigned long)dev->bdl);
    bm8w(nabmbar, ICH_PCO_LVI, 0);

    dev->present  = 1;
    dev->volume   = 80;
    dev->next_lvi = 0;

    klog_write("ich_ac97: init OK\n");
    return 0;
}

/* ── Volume ──────────────────────────────────────────────────────────────── */
void ich_audio_set_volume(ich_dev_t *dev, uint8_t vol)
{
    if (!dev->present) return;
    dev->volume = vol;
    /* AC97 attenuation: 0=0 dB, 31=−46.5 dB (5-bit per channel) */
    uint8_t att = (uint8_t)((100U - vol) * 31U / 100U);
    uint16_t reg = (uint16_t)(((uint16_t)att << 8) | att);
    if (vol == 0) reg |= 0x8000;
    nam_w(dev->nambar, AC97_MASTER_VOL,  reg);
    nam_w(dev->nambar, AC97_PCM_OUT_VOL, reg);
}

/* ── Busy check ──────────────────────────────────────────────────────────── *
 * Primary: DCH=1 (DMA halted) in SR means done.
 * Secondary: CIV == next_buf (current index caught up) means done.
 * Fallback: if CR RPBM not set, DMA wasn't running → not busy.            */
int ich_audio_busy(ich_dev_t *dev)
{
    if (!dev->present) return 0;
    uint8_t cr = bm8(dev->nabmbar, ICH_PCO_CR);
    if (!(cr & ICH_CR_RPBM)) return 0;   /* DMA not running */
    uint8_t sr = bm8(dev->nabmbar, ICH_PCO_SR);
    if (sr & ICH_SR_DCH)  return 0;      /* DMA halted */
    return 1;
}

/* ── Stop ────────────────────────────────────────────────────────────────── */
void ich_audio_stop(ich_dev_t *dev)
{
    if (!dev->present) return;
    bm8w(dev->nabmbar, ICH_PCO_CR, 0);
    bm8w(dev->nabmbar, ICH_PCO_CR, ICH_CR_RR);  /* RR clears ALL regs incl. BDBAR */
    delay_us(5000);
    bm8w(dev->nabmbar, ICH_PCO_CR, 0);
    bm16w(dev->nabmbar, ICH_PCO_SR, 0x1C);
    /* RR wiped BDBAR — restore it */
    bm32w(dev->nabmbar, ICH_PCO_BDBAR, (uint32_t)(unsigned long)dev->bdl);
    for (int i = 0; i < ICH_BUF_SLOTS; i++)
        memset(dev->pcm_buf[i], 0, ICH_BUF_BYTES);
    dev->next_lvi = 0;
    bm8w(dev->nabmbar, ICH_PCO_LVI, 0);
}

/* ── Submit: 8-bit mono 22050 Hz → 16-bit stereo 48000 Hz ───────────────── *
 * Fills the next BDL entry (next_lvi, advancing mod 32), sets LVI,         *
 * and restarts DMA. VirtualBox uses a 32-entry ring, so we maintain all    *
 * 32 entries; PCM buffers rotate through ICH_BUF_SLOTS actual buffers.     */
int ich_audio_submit(ich_dev_t *dev, const uint8_t *pcm8, uint32_t n8)
{
    if (!dev->present) return -1;

    int     lvi  = dev->next_lvi;                /* BDL entry to fill */
    int     slot = lvi % ICH_BUF_SLOTS;          /* PCM buffer to use */
    int16_t *dst = dev->pcm_buf[slot];

    /* Nearest-neighbour resample 22050 → 48000 Hz (Q16 fixed-point) */
    const uint32_t STEP = (uint32_t)((uint64_t)22050U * 65536U / 48000U);
    uint32_t frac = 0, si = 0;
    int      out  = 0;

    while (out < ICH_BUF_SAMPLES && si < n8) {
        int16_t s = (int16_t)(((int32_t)pcm8[si] - 128) << 8);
        dst[out * 2]     = s;
        dst[out * 2 + 1] = s;
        out++;
        frac += STEP;
        while (frac >= 65536U) { frac -= 65536U; si++; }
    }
    while (out < ICH_BUF_SAMPLES) {
        dst[out * 2] = dst[out * 2 + 1] = 0;
        out++;
    }

    /* Update this BDL entry to point at the filled PCM buffer */
    dev->bdl[lvi].addr    = (uint32_t)(unsigned long)dst;
    dev->bdl[lvi].samples = (uint16_t)out;
    dev->bdl[lvi].flags   = ICH_BDL_BUP;

    /* Advance LVI to this entry and (re)start DMA */
    bm8w(dev->nabmbar, ICH_PCO_LVI, (uint8_t)lvi);
    bm16w(dev->nabmbar, ICH_PCO_SR, 0x1C);
    bm8w(dev->nabmbar, ICH_PCO_CR, ICH_CR_RPBM);

    dev->next_lvi = (lvi + 1) % ICH_BDL_ENTRIES;
    return 0;
}
