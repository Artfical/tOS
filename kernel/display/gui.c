#include "gui.h"
#include "terminal.h"
#include "mouse.h"
#include "string.h"
#include "version.h"
#include "io.h"

#define VGA_MEM ((uint16_t *)0xB8000)
#define VGA_W 80
#define VGA_H 25

static uint16_t make_vga(char c, uint8_t fg, uint8_t bg)
{
    return (uint16_t)c | (uint16_t)(fg | (bg << 4)) << 8;
}

static int gui_active = 0;

int gui_is_active(void)
{
    return gui_active;
}

static int mouse_visible = 1;
static int prev_mx = -1, prev_my = -1;
static uint16_t prev_cell = 0;

void gui_draw_titlebar(void)
{
    for (int x = 0; x < VGA_W; x++)
        VGA_MEM[GUI_TITLE_ROW * VGA_W + x] = make_vga(' ', VGA_WHITE, VGA_BLUE);

    const char *title = "[X] tOS Terminal v" TOS_VERSION;
    int len = strlen(title);
    int start_x = (VGA_W - len) / 2;
    if (start_x < 0) start_x = 0;

    VGA_MEM[GUI_TITLE_ROW * VGA_W + 0] = make_vga('[', VGA_LIGHT_RED, VGA_BLUE);
    VGA_MEM[GUI_TITLE_ROW * VGA_W + 1] = make_vga('X', VGA_LIGHT_RED, VGA_BLUE);
    VGA_MEM[GUI_TITLE_ROW * VGA_W + 2] = make_vga(']', VGA_LIGHT_RED, VGA_BLUE);

    for (int i = 0; i < len; i++) {
        int pos = start_x + i;
        if (pos >= 0 && pos < VGA_W)
            VGA_MEM[GUI_TITLE_ROW * VGA_W + pos] = make_vga(title[i], VGA_WHITE, VGA_BLUE);
    }

    VGA_MEM[GUI_TITLE_ROW * VGA_W + VGA_W - 1] = make_vga(' ', VGA_WHITE, VGA_BLUE);
}

static void restore_prev_cell(void)
{
    if (prev_mx >= 0 && prev_my >= 0 && prev_mx < VGA_W && prev_my < VGA_H)
        VGA_MEM[prev_my * VGA_W + prev_mx] = prev_cell;
}

static void draw_cursor_at(int mx, int my)
{
    if (mx >= 0 && mx < VGA_W && my >= 0 && my < VGA_H) {
        prev_cell = VGA_MEM[my * VGA_W + mx];
        uint16_t cell = VGA_MEM[my * VGA_W + mx];
        uint8_t fg = (cell >> 8) & 0x0F;
        uint8_t bg = ((cell >> 8) >> 4) & 0x0F;
        VGA_MEM[my * VGA_W + mx] = make_vga(219, bg, fg);
        prev_mx = mx;
        prev_my = my;
    }
}

void gui_update_mouse(void)
{
    if (!mouse_visible) return;

    restore_prev_cell();

    int mx, my;
    uint8_t btns;
    mouse_get_state(&mx, &my, &btns);

    draw_cursor_at(mx, my);
}

void gui_poll(void)
{
    if (!gui_active) return;
    gui_update_mouse();

    int cx, cy;
    if (mouse_get_click(&cx, &cy)) {
        if (cy == GUI_TITLE_ROW) {
            if (cx >= 0 && cx <= 2) {
                terminal_writestring("Shutting down...\n");
                uint8_t good = 0x02;
                while (good & 0x02)
                    good = inb(0x64);
                outb(0x64, 0xFE);
                asm volatile("hlt");
            }
        }
    }
}

void gui_init(void)
{
    mouse_init();
    gui_draw_titlebar();
    gui_active = 1;
}
