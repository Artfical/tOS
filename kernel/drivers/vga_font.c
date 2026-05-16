#include "vga_font.h"
#include "io.h"
#include <stdint.h>
#include <string.h>

#define FONT_ADDR ((volatile uint8_t *)0xA0000)
#define FONT_BYTES (256 * 16)

static uint8_t font_buf[FONT_BYTES];

static void font_read(void)
{
    uint8_t seq0, seq2, seq4, gc4, gc5, gc6;

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
    outb(0x3CE, 0x05); outb(0x3CF, 0x40);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);

    for (int i = 0; i < FONT_BYTES; i++)
        font_buf[i] = FONT_ADDR[i];

    outb(0x3C4, 0x00); outb(0x3C5, seq0);
    outb(0x3C4, 0x02); outb(0x3C5, seq2);
    outb(0x3C4, 0x04); outb(0x3C5, seq4);
    outb(0x3CE, 0x04); outb(0x3CF, gc4);
    outb(0x3CE, 0x05); outb(0x3CF, gc5);
    outb(0x3CE, 0x06); outb(0x3CF, gc6);
}

static void font_write(void)
{
    uint8_t seq0, seq2, seq4, gc5, gc6;

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

void vga_font_load_turkish(void)
{
    font_read();

    uint8_t *c_ptr  = font_buf + 'c' * 16;
    uint8_t *C_ptr  = font_buf + 'C' * 16;
    uint8_t *g_ptr  = font_buf + 'g' * 16;
    uint8_t *G_ptr  = font_buf + 'G' * 16;
    uint8_t *i_ptr  = font_buf + 'i' * 16;
    uint8_t *I_ptr  = font_buf + 'I' * 16;
    uint8_t *o_ptr  = font_buf + 'o' * 16;
    uint8_t *O_ptr  = font_buf + 'O' * 16;
    uint8_t *s_ptr  = font_buf + 's' * 16;
    uint8_t *S_ptr  = font_buf + 'S' * 16;
    uint8_t *u_ptr  = font_buf + 'u' * 16;
    uint8_t *U_ptr  = font_buf + 'U' * 16;

    uint8_t *g_cc   = font_buf + 0xE7 * 16; // ç
    uint8_t *g_cC   = font_buf + 0xC7 * 16; // Ç
    uint8_t *g_gg   = font_buf + 0xF0 * 16; // ğ
    uint8_t *g_gG   = font_buf + 0xD0 * 16; // Ğ
    uint8_t *g_ii   = font_buf + 0xFD * 16; // ı
    uint8_t *g_iI   = font_buf + 0xDD * 16; // İ
    uint8_t *g_oo   = font_buf + 0xF6 * 16; // ö
    uint8_t *g_oO   = font_buf + 0xD6 * 16; // Ö
    uint8_t *g_ss   = font_buf + 0xFE * 16; // ş
    uint8_t *g_sS   = font_buf + 0xDE * 16; // Ş
    uint8_t *g_uu   = font_buf + 0xFC * 16; // ü
    uint8_t *g_uU   = font_buf + 0xDC * 16; // Ü

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

    font_write();
}
