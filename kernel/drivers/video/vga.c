#include "vga.h"
#include "io.h"
#include "serial.h"
#include "vga_font.h"

/* Mode set is done by programming the VGA controller's registers
 * directly (Misc Output, Sequencer, CRTC, Graphics Controller,
 * Attribute Controller) instead of a BIOS int 0x10 call, since this
 * kernel is already running in 32-bit protected mode by the time
 * vga_set_mode() can be called -- there's no BIOS to call into
 * without a v8086 monitor. These are the standard, widely-published
 * register values for mode 0x13 (320x200x256, chained) and mode 0x03
 * (80x25 text) respectively; see the OSDev wiki "VGA Hardware" page.
 */

static const uint8_t g_320x200x256[] = {
    /* MISC */
    0x63,
    /* SEQ index 0-4 */
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC index 0-0x18 */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
    /* GC index 0-8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    /* AC index 0-0x14 */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

#define VGA_REGSET_LEN 61 /* 1 misc + 5 seq + 25 crtc + 9 gc + 21 ac */
#define VGA_AC_OFFSET  40 /* 1 + 5 + 25 + 9 -- where the AC block starts */

/* The Attribute Controller has no dedicated read-index port (writes
 * use 0x3C0 for both index and data via an internal flip-flop, but
 * only the *data* half is readable back, from 0x3C1) and its readback
 * behavior under QEMU's std VGA emulation turned out to be unreliable
 * enough that round-tripping captured AC values corrupted the screen
 * on restore. Every other block (MISC/SEQ/CRTC/GC) has clean, working
 * dedicated read ports and round-trips fine, so those are captured
 * live from whatever valid text-mode state GRUB/the BIOS already left
 * the card in; only the AC block uses the fixed, standard mode-3
 * values below. */
static const uint8_t g_ac_text[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* Restoring text mode from a *captured* boot-time register snapshot
 * (below) still left the screen garbled after a DOOM/vgatest/3d
 * session on both QEMU and VirtualBox, even after also fixing the DAC
 * palette (see restore_dac_text() below) -- suggesting the capture
 * itself isn't trustworthy either (matching the AC block's already-
 * documented readback problems above), not just the write-back. This
 * is the same standard, fully-specified mode 0x03 (80x25 text)
 * register table every VGA BIOS uses, fully hardcoded like
 * g_320x200x256 above, instead of trusting any live capture. */
static const uint8_t g_80x25_text[] = {
    /* MISC */
    0x67,
    /* SEQ index 0-4 */
    0x03, 0x00, 0x03, 0x00, 0x02,
    /* CRTC index 0-0x18 */
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF,
    /* GC index 0-8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    /* AC index 0-0x14 */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static uint8_t g_boot_text_regs[VGA_REGSET_LEN];
static int g_boot_text_saved = 0;

/* The DAC's 256-entry RGB color table (ports 0x3C7 read-index/0x3C8
 * write-index, 0x3C9 data) is a *separate* piece of hardware state
 * from anything above -- the Attribute Controller only maps a 4-bit
 * attribute nibble to one of the DAC's first 64 entries, it doesn't
 * hold actual RGB values itself. Bochs/VBE's higher-bpp linear
 * framebuffer modes (DOOM/vgatest use 32bpp) address pixels directly
 * by RGB and have no reason to touch the legacy indexed-color DAC at
 * all, but evidently leave it in some other, non-standard state
 * anyway (observed as text rendering with correct glyphs/positions --
 * i.e. correct geometry -- but essentially random per-character
 * colors after returning to text mode, on both QEMU and VirtualBox).
 * A first attempt captured and restored the DAC live, the same way
 * the register blocks above do -- didn't fix it, meaning DAC readback
 * is apparently just as unreliable under emulation as the Attribute
 * Controller's already-documented readback problem above (the reason
 * g_ac_text is a fixed table instead of a live capture). Only DAC
 * indices 0-15 actually matter for text mode (that's all g_ac_text's
 * palette entries ever select), so this uses the same fix: skip
 * readback entirely and write the standard, well-known VGA 16-color
 * values every VGA BIOS uses for text mode. */
static const uint8_t g_dac_text[16 * 3] = {
     0,  0,  0,   0,  0, 42,   0, 42,  0,   0, 42, 42,
    42,  0,  0,  42,  0, 42,  42, 21,  0,  42, 42, 42,
    21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
    63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63,
};

static uint8_t g_current_mode = VGA_MODE_TEXT;

static void write_regs(const uint8_t *regs)
{
    int i = 0;

    outb(VGA_MISC_OUT, regs[i++]);

    for (int r = 0; r < 5; r++) {
        outb(VGA_SEQ_ADDR, (uint8_t)r);
        outb(VGA_SEQ_DATA, regs[i++]);
    }

    /* CRTC registers 0-7 are write-protected by default (index 0x11
     * bit 7, Vertical Retrace End); unlock before touching them. */
    outb(VGA_CRTC_ADDR, 0x11);
    outb(VGA_CRTC_DATA, inb(VGA_CRTC_DATA) & ~0x80);

    for (int r = 0; r < 25; r++) {
        outb(VGA_CRTC_ADDR, (uint8_t)r);
        outb(VGA_CRTC_DATA, regs[i++]);
    }

    for (int r = 0; r < 9; r++) {
        outb(VGA_GC_ADDR, (uint8_t)r);
        outb(VGA_GC_DATA, regs[i++]);
    }

    /* Attribute Controller uses a single port with an internal
     * address/data flip-flop; reading the input status register
     * resets it to the "address" state. */
    (void)inb(VGA_INSTAT);
    for (int r = 0; r < 21; r++) {
        outb(VGA_AC_ADDR, (uint8_t)r);
        outb(VGA_AC_ADDR, regs[i++]);
    }
    /* Set bit 5 (PAS) to re-enable video output. */
    outb(VGA_AC_ADDR, 0x20);
}

static void capture_current_regs(uint8_t *out)
{
    int i = 0;

    out[i++] = inb(0x3CC); /* Misc Output read-back port */

    for (int r = 0; r < 5; r++) {
        outb(VGA_SEQ_ADDR, (uint8_t)r);
        out[i++] = inb(VGA_SEQ_DATA);
    }

    for (int r = 0; r < 25; r++) {
        outb(VGA_CRTC_ADDR, (uint8_t)r);
        out[i++] = inb(VGA_CRTC_DATA);
    }

    for (int r = 0; r < 9; r++) {
        outb(VGA_GC_ADDR, (uint8_t)r);
        out[i++] = inb(VGA_GC_DATA);
    }

    /* AC index/read use separate ports (0x3C0 for index, 0x3C1 for
     * data) unlike every other register block here where the same
     * port serves both after selecting an index. */
    (void)inb(VGA_INSTAT);
    for (int r = 0; r < 21; r++) {
        outb(VGA_AC_ADDR, (uint8_t)r);
        out[i++] = inb(VGA_AC_DATA);
    }
}

static void restore_dac_text(void)
{
    outb(0x3C8, 0); /* DAC write index */
    for (int i = 0; i < 16 * 3; i++) outb(0x3C9, g_dac_text[i]);
}

void vga_init(void)
{
    if (!g_boot_text_saved) {
        capture_current_regs(g_boot_text_regs);
        for (int k = 0; k < 21; k++) g_boot_text_regs[VGA_AC_OFFSET + k] = g_ac_text[k];
        g_boot_text_saved = 1;
    }
}

void vga_set_mode(uint8_t mode)
{
    /* Holds interrupts off across the entire mode switch, not just the
     * register writes here -- GUI mode's desktop task keeps repainting
     * 0xB8000 on every preemptive timer tick regardless of what this
     * function is doing, and a timer interrupt landing mid-sequence
     * (mode-register writes are individually meaningless until they're
     * all applied together, and vga_font_set_style() below has its own
     * brief plane-2 addressing window) could switch to that repaint
     * task and corrupt the screen/font depending on exactly when it
     * landed -- observed as the desktop flickering between garbled and
     * correct after a DOOM/vgatest/3d session. Uses pushfl/popfl
     * (nests safely with vga_font_set_style()'s own interrupt-disable,
     * unlike a bare cli/sti pair, whose inner sti would re-enable
     * interrupts early and reopen the same race for the rest of this
     * function). */
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));

    if (!g_boot_text_saved) {
        capture_current_regs(g_boot_text_regs);
        for (int k = 0; k < 21; k++) g_boot_text_regs[VGA_AC_OFFSET + k] = g_ac_text[k];
        g_boot_text_saved = 1;
    }

    if (mode == VGA_MODE_320x200) {
        write_regs(g_320x200x256);
        g_current_mode = VGA_MODE_320x200;
    } else {
        write_regs(g_80x25_text);
        restore_dac_text();
        /* The character glyph bitmaps in VGA plane 2 are VRAM
         * *content*, not a register -- nothing above touches them, and
         * they don't survive a Bochs/VBE session intact (observed as
         * every character rendering as the same garbled/repeating
         * glyph shape afterward, despite geometry, register state, and
         * the DAC palette all being correctly restored above).
         * vga_font_set_style() unconditionally rewrites plane 2 from
         * its own cached copy of the real, BIOS/GRUB-loaded font
         * (captured once, well before this could ever run -- see
         * kernel.c's vga_font_capture_base() call during boot), so
         * calling it with whatever style is already active both fixes
         * this and preserves the user's chosen font style/Turkish
         * glyph patches across the switch. */
        vga_font_set_style(vga_font_get_style());
        g_current_mode = VGA_MODE_TEXT;
    }

    asm volatile("pushl %0; popfl" :: "r"(flags));
}

void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    /* Mode 13h is 256-color, indexed through the DAC (ports
     * 0x3C8/0x3C9), not the 16-color Attribute Controller palette --
     * each of R/G/B is a 6-bit (0-63) DAC value. */
    outb(0x3C8, index);
    outb(0x3C9, r & 0x3F);
    outb(0x3C9, g & 0x3F);
    outb(0x3C9, b & 0x3F);
}

void vga_write_pixel(int x, int y, uint8_t color)
{
    if (g_current_mode != VGA_MODE_320x200) return;
    if ((unsigned)x >= VGA_GFX_WIDTH || (unsigned)y >= VGA_GFX_HEIGHT) return;
    ((uint8_t *)0xA0000)[y * VGA_GFX_WIDTH + x] = color;
}

uint8_t *vga_get_framebuffer(void)
{
    return (uint8_t *)0xA0000;
}

/* Temporary debug helper for the mode 13h driver bring-up: dumps the
 * captured boot-time text-mode registers over serial so a bad
 * capture/restore can be spotted without guessing. Not for shipping. */
void vga_debug_dump_regs(void)
{
    if (!g_boot_text_saved) {
        capture_current_regs(g_boot_text_regs);
        for (int k = 0; k < 21; k++) g_boot_text_regs[VGA_AC_OFFSET + k] = g_ac_text[k];
        g_boot_text_saved = 1;
    }
    serial_write("vga: captured boot text regs:\n");
    const char *labels[] = {"MISC"};
    (void)labels;
    for (int i = 0; i < VGA_REGSET_LEN; i++) {
        uint8_t v = g_boot_text_regs[i];
        serial_putchar("0123456789ABCDEF"[(v >> 4) & 0xF]);
        serial_putchar("0123456789ABCDEF"[v & 0xF]);
        serial_putchar(' ');
        if (i % 10 == 9) serial_putchar('\n');
    }
    serial_putchar('\n');
}
