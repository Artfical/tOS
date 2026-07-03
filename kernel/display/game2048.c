#include "game2048.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"
#include "stdlib.h"
#include "string.h"

#define GRID_N      4
#define TILE_W      8
#define TILE_H      4
#define BOARD_X0    2
#define BOARD_Y0    3
#define STATUS_ROW  0

static int      grid[GRID_N][GRID_N];
static uint32_t score;
static uint32_t best_score;
static int      game_over;
static int      won_shown;

static uint8_t g_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void put_char_at(int x, int y, char c, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar(c);
}

static int fmt_uint(char *buf, unsigned int v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static uint8_t tile_color(int v)
{
    switch (v) {
        case 0:    return g_color(VGA_LIGHT_GREY, VGA_BLACK);
        case 2:    return g_color(VGA_BLACK, VGA_LIGHT_GREY);
        case 4:    return g_color(VGA_BLACK, VGA_WHITE);
        case 8:    return g_color(VGA_WHITE, VGA_LIGHT_RED);
        case 16:   return g_color(VGA_WHITE, VGA_RED);
        case 32:   return g_color(VGA_WHITE, VGA_LIGHT_BROWN);
        case 64:   return g_color(VGA_WHITE, VGA_BROWN);
        case 128:  return g_color(VGA_BLACK, VGA_LIGHT_GREEN);
        case 256:  return g_color(VGA_BLACK, VGA_GREEN);
        case 512:  return g_color(VGA_WHITE, VGA_LIGHT_CYAN);
        case 1024: return g_color(VGA_WHITE, VGA_CYAN);
        default:   return g_color(VGA_WHITE, VGA_LIGHT_MAGENTA);
    }
}

static int count_empty(void)
{
    int n = 0;
    for (int y = 0; y < GRID_N; y++)
        for (int x = 0; x < GRID_N; x++)
            if (grid[y][x] == 0) n++;
    return n;
}

static void spawn_tile(void)
{
    int empty = count_empty();
    if (empty == 0) return;
    int pick = rand() % empty;
    for (int y = 0; y < GRID_N; y++) {
        for (int x = 0; x < GRID_N; x++) {
            if (grid[y][x] != 0) continue;
            if (pick == 0) { grid[y][x] = (rand() % 10 == 0) ? 4 : 2; return; }
            pick--;
        }
    }
}

static void reset_game(void)
{
    memset(grid, 0, sizeof(grid));
    score = 0;
    game_over = 0;
    won_shown = 0;
    spawn_tile();
    spawn_tile();
}

/* Compresses+merges one line of 4 cells toward index 0 (the caller is
 * responsible for presenting the line in the right order/orientation for
 * whichever of the four move directions is being applied). Returns 1 if
 * the line changed. */
static int compress_merge(int line[GRID_N])
{
    int tmp[GRID_N];
    int n = 0;
    for (int i = 0; i < GRID_N; i++) if (line[i] != 0) tmp[n++] = line[i];
    for (int i = n; i < GRID_N; i++) tmp[i] = 0;

    for (int i = 0; i < GRID_N - 1; i++) {
        if (tmp[i] != 0 && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            score += (uint32_t)tmp[i];
            for (int j = i + 1; j < GRID_N - 1; j++) tmp[j] = tmp[j + 1];
            tmp[GRID_N - 1] = 0;
        }
    }

    int changed = 0;
    for (int i = 0; i < GRID_N; i++) { if (tmp[i] != line[i]) changed = 1; line[i] = tmp[i]; }
    return changed;
}

static int move_left(void)
{
    int changed = 0;
    for (int y = 0; y < GRID_N; y++) if (compress_merge(grid[y])) changed = 1;
    return changed;
}

static int move_right(void)
{
    int changed = 0;
    for (int y = 0; y < GRID_N; y++) {
        int line[GRID_N];
        for (int x = 0; x < GRID_N; x++) line[x] = grid[y][GRID_N - 1 - x];
        if (compress_merge(line)) changed = 1;
        for (int x = 0; x < GRID_N; x++) grid[y][GRID_N - 1 - x] = line[x];
    }
    return changed;
}

static int move_up(void)
{
    int changed = 0;
    for (int x = 0; x < GRID_N; x++) {
        int line[GRID_N];
        for (int y = 0; y < GRID_N; y++) line[y] = grid[y][x];
        if (compress_merge(line)) changed = 1;
        for (int y = 0; y < GRID_N; y++) grid[y][x] = line[y];
    }
    return changed;
}

static int move_down(void)
{
    int changed = 0;
    for (int x = 0; x < GRID_N; x++) {
        int line[GRID_N];
        for (int y = 0; y < GRID_N; y++) line[y] = grid[GRID_N - 1 - y][x];
        if (compress_merge(line)) changed = 1;
        for (int y = 0; y < GRID_N; y++) grid[GRID_N - 1 - y][x] = line[y];
    }
    return changed;
}

static int has_moves(void)
{
    if (count_empty() > 0) return 1;
    for (int y = 0; y < GRID_N; y++) {
        for (int x = 0; x < GRID_N; x++) {
            int v = grid[y][x];
            if (x + 1 < GRID_N && grid[y][x + 1] == v) return 1;
            if (y + 1 < GRID_N && grid[y + 1][x] == v) return 1;
        }
    }
    return 0;
}

static int has_2048(void)
{
    for (int y = 0; y < GRID_N; y++)
        for (int x = 0; x < GRID_N; x++)
            if (grid[y][x] >= 2048) return 1;
    return 0;
}

static void draw_status(void)
{
    char buf[96];
    int k = 0;
    const char *p = "Score: ";
    while (*p) buf[k++] = *p++;
    k += fmt_uint(buf + k, score);
    p = "   Best: ";
    while (*p) buf[k++] = *p++;
    k += fmt_uint(buf + k, best_score);
    p = "   Arrows to move, Enter to restart";
    while (*p) buf[k++] = *p++;
    if (game_over) {
        p = "   *** GAME OVER ***";
        while (*p) buf[k++] = *p++;
    } else if (won_shown) {
        p = "   *** 2048! Keep going or press Enter ***";
        while (*p) buf[k++] = *p++;
    }
    while (k < 78) buf[k++] = ' ';
    buf[k] = 0;
    put_str(0, STATUS_ROW, buf, g_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void draw_board(void)
{
    for (int y = 0; y < GRID_N; y++) {
        for (int x = 0; x < GRID_N; x++) {
            int v = grid[y][x];
            uint8_t c = tile_color(v);
            int px = BOARD_X0 + x * (TILE_W + 1);
            int py = BOARD_Y0 + y * (TILE_H + 1);

            for (int row = 0; row < TILE_H; row++)
                for (int col = 0; col < TILE_W; col++)
                    put_char_at(px + col, py + row, ' ', c);

            if (v != 0) {
                char numbuf[8];
                int n = fmt_uint(numbuf, (unsigned int)v);
                numbuf[n] = 0;
                int nx = px + (TILE_W - n) / 2;
                int ny = py + TILE_H / 2;
                put_str(nx, ny, numbuf, c);
            }
        }
    }
    draw_status();
    terminal_setcolor(g_color(VGA_LIGHT_GREY, VGA_BLACK));
}

void game2048_run(void)
{
    srand(task_get_ticks() ^ 0xC0FFEE);
    best_score = 0;
    reset_game();
    terminal_clear();
    draw_board();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int spec = keyboard_get_special();
        if (!game_over && spec) {
            int changed = 0;
            if (spec == 1) changed = move_left();
            else if (spec == 2) changed = move_right();
            else if (spec == 3) changed = move_up();
            else if (spec == 4) changed = move_down();

            if (changed) {
                spawn_tile();
                if (!won_shown && has_2048()) won_shown = 1;
                if (!has_moves()) {
                    game_over = 1;
                    if (score > best_score) best_score = score;
                }
                draw_board();
            }
        }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == '\n') {
                if (score > best_score) best_score = score;
                reset_game();
                draw_board();
            }
        }

        task_yield();
    }
}
