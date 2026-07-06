#include "vga_font.h"
#include "io.h"
#include "keyboard.h"
#include <stdint.h>
#include <string.h>

#define FONT_ADDR ((volatile uint8_t *)0xA0000)
#define FONT_BYTES (256 * 16)
#define GLYPH_ROWS 16

static uint8_t font_buf[FONT_BYTES];
static uint8_t base_font[FONT_BYTES];
static int base_captured = 0;
static int current_style = 0;

/* Selecting VGA plane 2 (where the character generator's glyph
 * bitmaps live) requires clearing GC5's "Host Odd/Even" bit (0x10) as
 * well as its "Read Mode" bit (0x08) -- masking off only the latter
 * (an earlier version of this function did) leaves Odd/Even active,
 * which overrides Read Map Select/Map Mask entirely and makes a
 * "plane 2" read/write actually go through the normal interleaved
 * text-mode character+attribute addressing instead. That bug meant
 * font_read() was capturing literal on-screen character+attribute
 * byte pairs (e.g. 0x20 0x07 for a blank space cell) instead of real
 * glyph bitmaps, silently, since it never gets compared against
 * anything -- only noticed once vga.c started relying on this cache
 * to restore glyphs after a DOOM/vgatest/3d session (see its
 * vga_font_set_style() call), where the wrong cached "glyphs" made
 * every character on screen render as visual noise. GC5 is fully
 * zeroed below (Write Mode 0, Read Mode 0, no Odd/Even) rather than
 * masking a single bit, matching the standard, widely-published VGA
 * font access technique.
 *
 * Both functions run with interrupts disabled for their entire
 * register-reprogram/copy/restore sequence: the VGA registers they
 * touch are shared machine-global state, not per-task. GUI mode's
 * desktop task keeps repainting via the normal preemptive timer tick
 * regardless of what any other task is doing, so without this, a
 * timer interrupt landing mid-sequence (while plane 2 is temporarily
 * selected) could switch to that repaint task, which writes to
 * 0xB8000 assuming ordinary interleaved text-mode addressing -- those
 * writes would actually land in font memory instead, corrupting the
 * glyph table and/or the screen, and differently depending on exactly
 * when the interrupt landed (observed as the desktop flickering
 * between garbled and correct after a DOOM/vgatest/3d session, worse
 * than the plain CLI shell -- which has no other task competing for
 * these registers -- ever showed). Uses pushfl/popfl rather than a
 * bare cli/sti pair so this nests safely: vga.c's vga_set_mode() also
 * needs to hold interrupts off across its own register writes plus
 * this call, and a plain inner sti would re-enable them early and
 * reopen the same race for the rest of vga_set_mode()'s work. */
static void font_read(void)
{
    uint8_t seq0, seq2, seq4, gc4, gc5, gc6;
    uint32_t flags;

    asm volatile("pushfl; popl %0; cli" : "=r"(flags));

    outb(0x3C4, 0x00); seq0 = inb(0x3C5);
    outb(0x3C4, 0x02); seq2 = inb(0x3C5);
    outb(0x3C4, 0x04); seq4 = inb(0x3C5);
    outb(0x3CE, 0x04); gc4 = inb(0x3CF);
    outb(0x3CE, 0x05); gc5 = inb(0x3CF);
    outb(0x3CE, 0x06); gc6 = inb(0x3CF);

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);

    for (int i = 0; i < FONT_BYTES; i++)
        font_buf[i] = FONT_ADDR[i];

    outb(0x3C4, 0x00); outb(0x3C5, seq0);
    outb(0x3C4, 0x02); outb(0x3C5, seq2);
    outb(0x3C4, 0x04); outb(0x3C5, seq4);
    outb(0x3CE, 0x04); outb(0x3CF, gc4);
    outb(0x3CE, 0x05); outb(0x3CF, gc5);
    outb(0x3CE, 0x06); outb(0x3CF, gc6);

    asm volatile("pushl %0; popfl" :: "r"(flags));
}

static void font_write(void)
{
    uint8_t seq0, seq2, seq4, gc5, gc6;
    uint32_t flags;

    asm volatile("pushfl; popl %0; cli" : "=r"(flags));

    outb(0x3C4, 0x00); seq0 = inb(0x3C5);
    outb(0x3C4, 0x02); seq2 = inb(0x3C5);
    outb(0x3C4, 0x04); seq4 = inb(0x3C5);
    outb(0x3CE, 0x05); gc5 = inb(0x3CF);
    outb(0x3CE, 0x06); gc6 = inb(0x3CF);

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);

    for (int i = 0; i < FONT_BYTES; i++)
        FONT_ADDR[i] = font_buf[i];

    outb(0x3C4, 0x00); outb(0x3C5, seq0);
    outb(0x3C4, 0x02); outb(0x3C5, seq2);
    outb(0x3C4, 0x04); outb(0x3C5, seq4);
    outb(0x3CE, 0x05); outb(0x3CF, gc5);
    outb(0x3CE, 0x06); outb(0x3CF, gc6);

    asm volatile("pushl %0; popfl" :: "r"(flags));
}

static void add_cedilla_below(uint8_t *glyph)
{
    glyph[13] = 0x0C;
    glyph[14] = 0x18;
}

static void add_breve_above(uint8_t *glyph)
{
    glyph[1] = 0x66;
    glyph[2] = 0x00;
}

static void add_umlaut_above(uint8_t *glyph)
{
    glyph[1] = 0x24;
    glyph[2] = 0x00;
}

static void remove_i_dot(uint8_t *glyph)
{
    glyph[2] = 0x00;
}

static void add_i_dot_above(uint8_t *glyph)
{
    glyph[1] = 0x18;
    glyph[2] = 0x00;
}

static void apply_turkish_patches(uint8_t *buf)
{
    uint8_t *c_ptr  = buf + 'c' * 16;
    uint8_t *C_ptr  = buf + 'C' * 16;
    uint8_t *g_ptr  = buf + 'g' * 16;
    uint8_t *G_ptr  = buf + 'G' * 16;
    uint8_t *i_ptr  = buf + 'i' * 16;
    uint8_t *I_ptr  = buf + 'I' * 16;
    uint8_t *o_ptr  = buf + 'o' * 16;
    uint8_t *O_ptr  = buf + 'O' * 16;
    uint8_t *s_ptr  = buf + 's' * 16;
    uint8_t *S_ptr  = buf + 'S' * 16;
    uint8_t *u_ptr  = buf + 'u' * 16;
    uint8_t *U_ptr  = buf + 'U' * 16;

    uint8_t *g_cc   = buf + 0xE7 * 16; // ç
    uint8_t *g_cC   = buf + 0xC7 * 16; // Ç
    uint8_t *g_gg   = buf + 0xF0 * 16; // ğ
    uint8_t *g_gG   = buf + 0xD0 * 16; // Ğ
    uint8_t *g_ii   = buf + 0xFD * 16; // ı
    uint8_t *g_iI   = buf + 0xDD * 16; // İ
    uint8_t *g_oo   = buf + 0xF6 * 16; // ö
    uint8_t *g_oO   = buf + 0xD6 * 16; // Ö
    uint8_t *g_ss   = buf + 0xFE * 16; // ş
    uint8_t *g_sS   = buf + 0xDE * 16; // Ş
    uint8_t *g_uu   = buf + 0xFC * 16; // ü
    uint8_t *g_uU   = buf + 0xDC * 16; // Ü

    memcpy(g_cc, c_ptr, 16); add_cedilla_below(g_cc);
    memcpy(g_cC, C_ptr, 16); add_cedilla_below(g_cC);
    memcpy(g_gg, g_ptr, 16); add_breve_above(g_gg);
    memcpy(g_gG, G_ptr, 16); add_breve_above(g_gG);
    memcpy(g_ii, i_ptr, 16); remove_i_dot(g_ii);
    memcpy(g_iI, I_ptr, 16); add_i_dot_above(g_iI);
    memcpy(g_oo, o_ptr, 16); add_umlaut_above(g_oo);
    memcpy(g_oO, O_ptr, 16); add_umlaut_above(g_oO);
    memcpy(g_ss, s_ptr, 16); add_cedilla_below(g_ss);
    memcpy(g_sS, S_ptr, 16); add_cedilla_below(g_sS);
    memcpy(g_uu, u_ptr, 16); add_umlaut_above(g_uu);
    memcpy(g_uU, U_ptr, 16); add_umlaut_above(g_uU);
}

static void ensure_base_captured(void)
{
    if (base_captured) return;
    font_read();
    memcpy(base_font, font_buf, FONT_BYTES);
    base_captured = 1;
}

static const char *style_names[VGA_FONT_STYLE_COUNT] = {
    "default", "bold", "narrow", "italic", "underline",
    "strikethrough", "wide", "outline", "shadow", "retro",
};

static void apply_style(int style, uint8_t *dst)
{
    for (int g = 0; g < 256; g++) {
        const uint8_t *src = base_font + g * GLYPH_ROWS;
        uint8_t *out = dst + g * GLYPH_ROWS;
        for (int r = 0; r < GLYPH_ROWS; r++) {
            uint8_t row = src[r];
            switch (style) {
                case 1: /* bold */
                    out[r] = row | (row >> 1);
                    break;
                case 2: /* narrow */
                    out[r] = row & 0xFE;
                    break;
                case 3: /* italic */
                    if (r < 4) out[r] = row >> 1;
                    else if (r >= 12) out[r] = row << 1;
                    else out[r] = row;
                    break;
                case 4: /* underline */
                    out[r] = (r == 14) ? 0xFF : row;
                    break;
                case 5: /* strikethrough */
                    out[r] = (r == 8) ? 0xFF : row;
                    break;
                case 6: /* wide */
                    out[r] = row | (row >> 1) | (row << 1);
                    break;
                case 7: /* outline */
                    out[r] = row & (uint8_t)~(row << 1) & (uint8_t)~(row >> 1);
                    break;
                case 8: /* shadow */
                    out[r] = row | (r > 0 ? (src[r - 1] >> 1) : 0);
                    break;
                case 9: /* retro */
                    out[r] = row | (r < GLYPH_ROWS - 1 ? src[r + 1] : 0) | (row >> 1);
                    break;
                default: /* default */
                    out[r] = row;
                    break;
            }
        }
    }
}

void vga_font_capture_base(void)
{
    ensure_base_captured();
}

void vga_font_set_style(int style)
{
    if (style < 0 || style >= VGA_FONT_STYLE_COUNT) style = 0;
    ensure_base_captured();
    apply_style(style, font_buf);
    if (keyboard_get_layout() == KBD_LAYOUT_TRQ)
        apply_turkish_patches(font_buf);
    font_write();
    current_style = style;
}

int vga_font_get_style(void)
{
    return current_style;
}

const char *vga_font_style_name(int style)
{
    if (style < 0 || style >= VGA_FONT_STYLE_COUNT) return "?";
    return style_names[style];
}

void vga_font_load_turkish(void)
{
    ensure_base_captured();
    apply_style(current_style, font_buf);
    apply_turkish_patches(font_buf);
    font_write();
}
