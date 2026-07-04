#include "paint.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"

#define PAINT_COLS 79
#define PAINT_ROWS 22
#define TOOLBAR_ROW 0
#define SIZEBAR_ROW 1
#define PALETTE_ROW 2
#define CANVAS_Y0 3
#define STATUS_ROW (PAINT_ROWS - 1)
#define CANVAS_H (STATUS_ROW - CANVAS_Y0)
#define CANVAS_W PAINT_COLS

#define TOOL_PEN 0
#define TOOL_ERASER 1
#define TOOL_LINE 2
#define TOOL_RECT 3
#define TOOL_CIRCLE 4
#define CANVAS_BG VGA_WHITE
#define MAX_BRUSH 3

#define NUM_TOOLS 7
static const char *tool_labels[NUM_TOOLS] = { "Pen", "Eraser", "Line", "Rect", "Circle", "New", "Save" };
static int tool_x0[NUM_TOOLS], tool_x1[NUM_TOOLS];

#define NUM_SIZES MAX_BRUSH
static const char *size_labels[NUM_SIZES] = { "1", "2", "3" };
static int size_x0[NUM_SIZES], size_x1[NUM_SIZES];

#define NUM_COLORS 16
static int pal_x0[NUM_COLORS], pal_x1[NUM_COLORS];

static uint8_t canvas[CANVAS_H][CANVAS_W];
static uint8_t shape_base[CANVAS_H][CANVAS_W];
static int cur_color;
static int brush_size;
static int tool;
static char filename[64];
static char status_msg[PAINT_COLS + 1];

static int shape_dragging;
static int shape_start_x, shape_start_y;

static uint8_t pcolor(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < PAINT_COLS) { status_msg[i] = s[i]; i++; }
    status_msg[i] = 0;
}

static void new_canvas(void)
{
    for (int r = 0; r < CANVAS_H; r++)
        for (int c = 0; c < CANVAS_W; c++)
            canvas[r][c] = CANVAS_BG;
    filename[0] = 0;
    set_status("New canvas.");
}

static void paint_at(int cx, int cy, uint8_t val)
{
    for (int dy = 0; dy < brush_size; dy++) {
        for (int dx = 0; dx < brush_size; dx++) {
            int yy = cy + dy, xx = cx + dx;
            if (yy >= 0 && yy < CANVAS_H && xx >= 0 && xx < CANVAS_W) canvas[yy][xx] = val;
        }
    }
}

static void stamp(int cx, int cy)
{
    paint_at(cx, cy, (tool == TOOL_ERASER) ? (uint8_t)CANVAS_BG : (uint8_t)cur_color);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static int isqrt(int v)
{
    if (v <= 0) return 0;
    int r = v;
    for (int i = 0; i < 20; i++) r = (r + v / r) / 2;
    return r;
}

static void draw_line_shape(int x0, int y0, int x1, int y1, uint8_t val)
{
    int dx = iabs(x1 - x0), dy = iabs(y1 - y0);
    int sx = (x1 >= x0) ? 1 : -1, sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    for (;;) {
        paint_at(x, y, val);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static void draw_rect_shape(int x0, int y0, int x1, int y1, uint8_t val)
{
    int xa = x0 < x1 ? x0 : x1, xb = x0 < x1 ? x1 : x0;
    int ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
    for (int x = xa; x <= xb; x++) { paint_at(x, ya, val); paint_at(x, yb, val); }
    for (int y = ya; y <= yb; y++) { paint_at(xa, y, val); paint_at(xb, y, val); }
}

static void draw_circle_shape(int cx, int cy, int ex, int ey, uint8_t val)
{
    int dx = ex - cx, dy = ey - cy;
    int r = isqrt(dx * dx + dy * dy);
    int x = r, y = 0, err = 0;
    while (x >= y) {
        paint_at(cx + x, cy + y, val); paint_at(cx + y, cy + x, val);
        paint_at(cx - y, cy + x, val); paint_at(cx - x, cy + y, val);
        paint_at(cx - x, cy - y, val); paint_at(cx - y, cy - x, val);
        paint_at(cx + y, cy - x, val); paint_at(cx + x, cy - y, val);
        y++;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) { x--; err += 1 - 2 * x; }
    }
}

static void draw_toolbar(void)
{
    put_str(0, TOOLBAR_ROW, "                                                                               ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    int x = 1;
    for (int i = 0; i < NUM_TOOLS; i++) {
        const char *lbl = tool_labels[i];
        int len = (int)strlen(lbl);
        int w = len + 4;
        tool_x0[i] = x;
        tool_x1[i] = x + w - 1;

        int active = (i <= TOOL_CIRCLE) && (tool == i);
        terminal_setpos((size_t)x, TOOLBAR_ROW);
        terminal_setcolor(active ? pcolor(VGA_WHITE, VGA_BLUE) : pcolor(VGA_BLACK, VGA_LIGHT_GREY));
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 1;
    }
}

static void draw_sizebar(void)
{
    put_str(0, SIZEBAR_ROW, "                                                                               ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    int x = 1;
    put_str(x, SIZEBAR_ROW, "Size:", pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    x += 6;
    for (int i = 0; i < NUM_SIZES; i++) {
        const char *lbl = size_labels[i];
        int len = (int)strlen(lbl);
        int w = len + 4;
        size_x0[i] = x;
        size_x1[i] = x + w - 1;

        int active = (brush_size == i + 1);
        terminal_setpos((size_t)x, SIZEBAR_ROW);
        terminal_setcolor(active ? pcolor(VGA_WHITE, VGA_BLUE) : pcolor(VGA_BLACK, VGA_LIGHT_GREY));
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 1;
    }
}

static void draw_palette(void)
{
    int x = 0;
    for (int i = 0; i < NUM_COLORS; i++) {
        pal_x0[i] = x;
        pal_x1[i] = x + 3;
        terminal_setpos((size_t)x, PALETTE_ROW);
        terminal_setcolor(pcolor((uint8_t)i, (uint8_t)i));
        for (int j = 0; j < 4; j++) terminal_putchar(' ');
        if (i == cur_color) {
            terminal_setpos((size_t)(x + 1), PALETTE_ROW);
            terminal_setcolor(pcolor(i == VGA_WHITE || i == VGA_LIGHT_GREY || i == VGA_LIGHT_CYAN || i == VGA_LIGHT_GREEN || i == VGA_LIGHT_BROWN ? VGA_BLACK : VGA_WHITE, (uint8_t)i));
            terminal_putchar((char)0xFE);
        }
        x += 5;
    }
}

static void draw_canvas(void)
{
    for (int r = 0; r < CANVAS_H; r++) {
        terminal_setpos(0, (size_t)(CANVAS_Y0 + r));
        for (int c = 0; c < CANVAS_W; c++) {
            terminal_setcolor(pcolor(canvas[r][c], canvas[r][c]));
            terminal_putchar(' ');
        }
    }
}

static void draw_status(void)
{
    char line[PAINT_COLS + 1];
    int k = 0;
    while (status_msg[k] && k < PAINT_COLS) { line[k] = status_msg[k]; k++; }
    while (k < PAINT_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_ROW, line, pcolor(VGA_BLACK, VGA_LIGHT_GREY));
}

static void redraw(void)
{
    draw_toolbar();
    draw_sizebar();
    draw_palette();
    draw_canvas();
    draw_status();
}

/* --- Minimal from-scratch PNG encoder: each canvas cell becomes a solid
 * PIXELS_PER_CELL x PIXELS_PER_CELL block of real RGB pixels, so the
 * saved file is a normal, valid PNG viewable on any machine even though
 * tOS itself never leaves VGA text mode to draw it. Uses "stored"
 * (uncompressed) deflate blocks inside a minimal zlib wrapper, since
 * implementing real DEFLATE compression isn't worth it for a paint
 * canvas this small. */
#define PIXELS_PER_CELL 6

static const uint8_t vga_palette[16][3] = {
    {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
    {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
    {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

static uint32_t crc32_table[256];
static int crc32_ready;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    if (!crc32_ready) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32_buf(const uint8_t *data, uint32_t len)
{
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t write_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t len)
{
    put_u32be(out, len);
    out[4] = (uint8_t)type[0]; out[5] = (uint8_t)type[1];
    out[6] = (uint8_t)type[2]; out[7] = (uint8_t)type[3];
    if (len && data) memcpy(out + 8, data, len);
    uint32_t crc = crc32_buf(out + 4, len + 4);
    put_u32be(out + 8 + len, crc);
    return 8 + len + 4;
}

static int export_png(const char *path)
{
    const int ppc = PIXELS_PER_CELL;
    uint32_t W = (uint32_t)(CANVAS_W * ppc);
    uint32_t H = (uint32_t)(CANVAS_H * ppc);
    uint32_t row_bytes = W * 3 + 1;
    uint32_t raw_size = row_bytes * H;

    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) return -1;

    for (int cy = 0; cy < CANVAS_H; cy++) {
        for (int py = 0; py < ppc; py++) {
            uint32_t y = (uint32_t)(cy * ppc + py);
            uint8_t *rowp = raw + (uint32_t)y * row_bytes;
            rowp[0] = 0;
            for (int cx = 0; cx < CANVAS_W; cx++) {
                const uint8_t *rgb = vga_palette[canvas[cy][cx] & 0xF];
                for (int px = 0; px < ppc; px++) {
                    uint32_t x = (uint32_t)(cx * ppc + px);
                    uint8_t *pp = rowp + 1 + x * 3;
                    pp[0] = rgb[0]; pp[1] = rgb[1]; pp[2] = rgb[2];
                }
            }
        }
    }

    uint32_t nblocks = (raw_size + 65534) / 65535;
    if (nblocks == 0) nblocks = 1;
    uint32_t zlib_size = 2 + nblocks * 5 + raw_size + 4;
    uint8_t *zbuf = (uint8_t *)malloc(zlib_size);
    if (!zbuf) { free(raw); return -1; }

    uint32_t zp = 0;
    zbuf[zp++] = 0x78; zbuf[zp++] = 0x01;
    uint32_t off = 0;
    while (off < raw_size) {
        uint32_t chunk = raw_size - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= raw_size);
        zbuf[zp++] = final ? 1 : 0;
        zbuf[zp++] = (uint8_t)(chunk & 0xFF);
        zbuf[zp++] = (uint8_t)((chunk >> 8) & 0xFF);
        uint16_t nlen = (uint16_t)~chunk;
        zbuf[zp++] = (uint8_t)(nlen & 0xFF);
        zbuf[zp++] = (uint8_t)((nlen >> 8) & 0xFF);
        memcpy(zbuf + zp, raw + off, chunk);
        zp += chunk;
        off += chunk;
    }
    uint32_t adler = adler32_buf(raw, raw_size);
    free(raw);
    zbuf[zp++] = (uint8_t)(adler >> 24); zbuf[zp++] = (uint8_t)(adler >> 16);
    zbuf[zp++] = (uint8_t)(adler >> 8);  zbuf[zp++] = (uint8_t)adler;

    uint8_t ihdr[13];
    put_u32be(ihdr + 0, W);
    put_u32be(ihdr + 4, H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    uint32_t file_cap = 8 + (4 + 4 + 13 + 4) + (4 + 4 + zp + 4) + (4 + 4 + 4);
    uint8_t *file = (uint8_t *)malloc(file_cap);
    if (!file) { free(zbuf); return -1; }

    static const uint8_t sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    uint32_t fp = 0;
    memcpy(file + fp, sig, 8); fp += 8;
    fp += write_chunk(file + fp, "IHDR", ihdr, 13);
    fp += write_chunk(file + fp, "IDAT", zbuf, zp);
    fp += write_chunk(file + fp, "IEND", 0, 0);
    free(zbuf);

    if (fsbridge_exists(path)) fsbridge_delete(path);
    int ok = (fsbridge_create(path) == 0);
    if (ok) ok = (fsbridge_write(path, file, fp, 0) >= 0);
    free(file);
    return ok ? 0 : -1;
}

static void ensure_png_extension(char *name, int max)
{
    int len = (int)strlen(name);
    if (len == 0) return;
    for (int i = 0; i < len; i++) if (name[i] == '.') return;
    if (len + 4 < max) strcat(name, ".png");
}

static void prompt_filename(const char *prompt, char *buf, int max)
{
    put_str(0, STATUS_ROW, "                                                                               ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_ROW, prompt, pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)strlen(prompt), (size_t)STATUS_ROW);
    keyboard_readline(buf, max);
}

static void do_save(void)
{
    char fname[64];
    if (filename[0]) {
        strncpy(fname, filename, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = 0;
    } else {
        prompt_filename("Save as: ", fname, sizeof(fname));
        if (!fname[0]) { status_msg[0] = 0; return; }
        ensure_png_extension(fname, sizeof(fname));
    }

    if (export_png(fname) == 0) {
        strncpy(filename, fname, sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = 0;
        set_status("Saved.");
    } else {
        set_status("Save failed.");
    }
}

static void do_new(void)
{
    put_str(0, STATUS_ROW, "Clear canvas? (y/n)                                                           ",
            pcolor(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos(20, (size_t)STATUS_ROW);
    if (keyboard_yesno()) new_canvas();
    else status_msg[0] = 0;
}

static void handle_toolbar_click(int ccx)
{
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (ccx >= tool_x0[i] && ccx <= tool_x1[i]) {
            if (i <= TOOL_CIRCLE) { tool = i; shape_dragging = 0; }
            else if (i == 5) do_new();
            else if (i == 6) do_save();
            return;
        }
    }
}

static void handle_sizebar_click(int ccx)
{
    for (int i = 0; i < NUM_SIZES; i++) {
        if (ccx >= size_x0[i] && ccx <= size_x1[i]) { brush_size = i + 1; return; }
    }
}

static void handle_palette_click(int ccx)
{
    for (int i = 0; i < NUM_COLORS; i++) {
        if (ccx >= pal_x0[i] && ccx <= pal_x1[i]) { cur_color = i; return; }
    }
}

void paint_run(void)
{
    new_canvas();
    cur_color = VGA_BLACK;
    brush_size = 1;
    tool = TOOL_PEN;
    shape_dragging = 0;
    status_msg[0] = 0;

    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == TOOLBAR_ROW) handle_toolbar_click(ccx);
            else if (ccy == SIZEBAR_ROW) handle_sizebar_click(ccx);
            else if (ccy == PALETTE_ROW) handle_palette_click(ccx);
            else if (ccy >= CANVAS_Y0 && ccy < CANVAS_Y0 + CANVAS_H && tool <= TOOL_ERASER)
                stamp(ccx, ccy - CANVAS_Y0);
            redraw();
            task_yield();
            continue;
        }

        int mx, my, btns;
        int got_mouse = wm_get_content_mouse(&mx, &my, &btns);
        int held = got_mouse && (btns & 1);
        int in_canvas = held && my >= CANVAS_Y0 && my < CANVAS_Y0 + CANVAS_H && mx >= 0 && mx < CANVAS_W;

        if (tool <= TOOL_ERASER) {
            if (in_canvas) { stamp(mx, my - CANVAS_Y0); draw_canvas(); }
        } else {
            if (in_canvas) {
                int ex = mx, ey = my - CANVAS_Y0;
                if (!shape_dragging) {
                    shape_dragging = 1;
                    shape_start_x = ex;
                    shape_start_y = ey;
                    memcpy(shape_base, canvas, sizeof(canvas));
                }
                memcpy(canvas, shape_base, sizeof(canvas));
                if (tool == TOOL_LINE) draw_line_shape(shape_start_x, shape_start_y, ex, ey, (uint8_t)cur_color);
                else if (tool == TOOL_RECT) draw_rect_shape(shape_start_x, shape_start_y, ex, ey, (uint8_t)cur_color);
                else if (tool == TOOL_CIRCLE) draw_circle_shape(shape_start_x, shape_start_y, ex, ey, (uint8_t)cur_color);
                draw_canvas();
            } else if (shape_dragging) {
                shape_dragging = 0;
            }
        }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == 'w' || c == 'W') {
                wm_set_wallpaper_from_cells(&canvas[0][0], CANVAS_H, CANVAS_W);
                set_status("Wallpaper set (not saved to a file).");
                redraw();
            }
        }

        task_yield();
    }
}
