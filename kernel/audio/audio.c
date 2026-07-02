#include "audio.h"
#include "sb16.h"
#include "ich.h"
#include "io.h"
#include "isr.h"
#include "string.h"

/* ── Backend selection ────────────────────────────────────────────────────── */
#define BACKEND_NONE  0
#define BACKEND_SB16  1
#define BACKEND_ICH   2

static int g_backend = BACKEND_NONE;

/* ── SB16 state ───────────────────────────────────────────────────────────── */
static sb16_device_t g_sb;
static volatile int  g_done = 1;
static uint8_t dma_buf[AUDIO_DMA_SIZE] __attribute__((aligned(AUDIO_DMA_SIZE)));

/* ── ICH AC97 state ───────────────────────────────────────────────────────── */
static ich_dev_t g_ich;

/* ── ISA DMA channel 1 (8-bit) programming ───────────────────────────────── */
static void dma_ch1_start(uint32_t phys, uint32_t count)
{
    uint8_t  page   = (uint8_t)(phys >> 16);
    uint16_t offset = (uint16_t)(phys & 0xFFFFU);
    uint16_t cnt    = (uint16_t)(count - 1U);

    outb(0x0A, 0x05);
    outb(0x0C, 0x00);
    outb(0x02, offset & 0xFF);
    outb(0x02, (uint8_t)(offset >> 8));
    outb(0x03, cnt & 0xFF);
    outb(0x03, (uint8_t)(cnt >> 8));
    outb(0x83, page);
    outb(0x0B, 0x49);
    outb(0x0A, 0x01);
}

/* ── IRQ5 handler (SB16 DMA done) ────────────────────────────────────────── */
static void sb16_irq5(registers_t *r)
{
    (void)r;
    inb((uint16_t)(g_sb.base + 0x0EU));
    g_done = 1;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void audio_init(void)
{
    /* Try SB16 first (QEMU -soundhw sb16) */
    g_sb.present = 0;
    if (sb16_init(&g_sb) == 0) {
        isr_register_handler(37, sb16_irq5);
        g_backend = BACKEND_SB16;
        audio_set_volume(80);
        return;
    }

    /* Fallback: ICH AC97 (VirtualBox, QEMU -device AC97) */
    if (ich_audio_init(&g_ich) == 0) {
        g_backend = BACKEND_ICH;
        audio_set_volume(80);
        return;
    }

    g_backend = BACKEND_NONE;
}

int audio_available(void)
{
    return g_backend != BACKEND_NONE;
}

const char *audio_backend_name(void)
{
    if (g_backend == BACKEND_SB16) return "SB16";
    if (g_backend == BACKEND_ICH)  return "AC97";
    return "None";
}

void audio_set_volume(uint8_t vol)
{
    if (g_backend == BACKEND_SB16) {
        uint8_t v = (uint8_t)(((uint32_t)vol * 15U) / 100U) & 0x0FU;
        outb((uint16_t)(g_sb.base + SB16_MIXER_ADDR), 0x22U);
        outb((uint16_t)(g_sb.base + SB16_MIXER_DATA), (uint8_t)((v << 4) | v));
    } else if (g_backend == BACKEND_ICH) {
        ich_audio_set_volume(&g_ich, vol);
    }
}

int audio_busy(void)
{
    if (g_backend == BACKEND_SB16) return !g_done;
    if (g_backend == BACKEND_ICH)  return ich_audio_busy(&g_ich);
    return 0;
}

void audio_stop(void)
{
    if (g_backend == BACKEND_SB16) {
        sb16_write(&g_sb, SB16_CMD_STOP_8BIT);
        g_done = 1;
    } else if (g_backend == BACKEND_ICH) {
        ich_audio_stop(&g_ich);
    }
}

int audio_submit(const uint8_t *pcm, uint32_t len)
{
    if (g_backend == BACKEND_SB16) {
        if (len > AUDIO_DMA_SIZE) len = AUDIO_DMA_SIZE;
        for (volatile int t = 0; t < 20000000 && !g_done; t++);
        if (!g_done) audio_stop();
        memcpy(dma_buf, pcm, len);
        g_done = 0;
        dma_ch1_start((uint32_t)(unsigned long)dma_buf, len);
        sb16_write(&g_sb, SB16_CMD_SET_TIME_CONSTANT);
        sb16_write(&g_sb, 211U);
        sb16_write(&g_sb, SB16_CMD_8BIT_PLAY);
        sb16_write(&g_sb, (uint8_t)((len - 1U) & 0xFFU));
        sb16_write(&g_sb, (uint8_t)((len - 1U) >> 8U));
        return 0;
    }
    if (g_backend == BACKEND_ICH) {
        return ich_audio_submit(&g_ich, pcm, len);
    }
    return -1;
}
