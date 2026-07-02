#include "audio.h"
#include "sb16.h"
#include "io.h"
#include "isr.h"
#include "string.h"

static sb16_device_t         g_sb;
static volatile int          g_done = 1;

/* DMA buffer: aligned to its own size so it never crosses a 64 KB boundary */
static uint8_t dma_buf[AUDIO_DMA_SIZE] __attribute__((aligned(AUDIO_DMA_SIZE)));

/* ── ISA DMA channel 1 (8-bit) programming ──────────────────────────────── */
static void dma_ch1_start(uint32_t phys, uint32_t count)
{
    uint8_t  page   = (uint8_t)(phys >> 16);
    uint16_t offset = (uint16_t)(phys & 0xFFFFU);
    uint16_t cnt    = (uint16_t)(count - 1U);

    outb(0x0A, 0x05);              /* mask DMA ch1          */
    outb(0x0C, 0x00);              /* clear flip-flop       */
    outb(0x02, offset & 0xFF);     /* address low           */
    outb(0x02, (uint8_t)(offset >> 8)); /* address high     */
    outb(0x03, cnt & 0xFF);        /* count low             */
    outb(0x03, (uint8_t)(cnt >> 8));    /* count high       */
    outb(0x83, page);              /* page register         */
    /* mode: single-cycle, read (mem→device), increment, ch1 */
    outb(0x0B, 0x49);
    outb(0x0A, 0x01);              /* unmask ch1            */
}

/* ── IRQ5 handler (SB16 DMA done) ────────────────────────────────────────── */
static void sb16_irq5(registers_t *r)
{
    (void)r;
    inb((uint16_t)(g_sb.base + 0x0EU)); /* acknowledge 8-bit IRQ */
    g_done = 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void audio_init(void)
{
    g_sb.present = 0;
    if (sb16_init(&g_sb) != 0) return;
    /* IRQ5 = PIC vector 32+5 = 37 */
    isr_register_handler(37, sb16_irq5);
    audio_set_volume(80);
}

int audio_available(void) { return g_sb.present; }

void audio_set_volume(uint8_t vol)
{
    if (!g_sb.present) return;
    /* SB16 mixer master volume: reg 0x22, high nibble = L, low nibble = R (0-15) */
    uint8_t v = (uint8_t)(((uint32_t)vol * 15U) / 100U) & 0x0FU;
    outb((uint16_t)(g_sb.base + SB16_MIXER_ADDR), 0x22U);
    outb((uint16_t)(g_sb.base + SB16_MIXER_DATA), (uint8_t)((v << 4) | v));
}

int audio_busy(void) { return !g_done; }

void audio_stop(void)
{
    if (!g_sb.present) return;
    sb16_write(&g_sb, SB16_CMD_STOP_8BIT);
    g_done = 1;
}

int audio_submit(const uint8_t *pcm, uint32_t len)
{
    if (!g_sb.present) return -1;
    if (len > AUDIO_DMA_SIZE) len = AUDIO_DMA_SIZE;

    /* spin-wait for previous chunk (≤ ~186ms, very short) */
    for (volatile int t = 0; t < 20000000 && !g_done; t++);
    if (!g_done) audio_stop();

    memcpy(dma_buf, pcm, len);
    g_done = 0;

    dma_ch1_start((uint32_t)(unsigned long)dma_buf, len);

    /* time_constant = 256 - 1000000/rate ≈ 211 for 22050 Hz */
    sb16_write(&g_sb, SB16_CMD_SET_TIME_CONSTANT);
    sb16_write(&g_sb, 211U);

    /* 8-bit single-cycle play */
    sb16_write(&g_sb, SB16_CMD_8BIT_PLAY);
    sb16_write(&g_sb, (uint8_t)((len - 1U) & 0xFFU));
    sb16_write(&g_sb, (uint8_t)((len - 1U) >> 8U));

    return 0;
}
