#include "snake.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "wm.h"
#include "scheduler.h"
#include "stdlib.h"
#include "string.h"

#define BOARD_X0    1
#define BOARD_Y0    2
#define BOARD_COLS  60
#define BOARD_ROWS  20
#define STATUS_ROW  0

#define DIR_LEFT  0
#define DIR_RIGHT 1
#define DIR_UP    2
#define DIR_DOWN  3

#define MAX_LEN (BOARD_COLS * BOARD_ROWS)

typedef struct { int x, y; } pt_t;

static pt_t snake[MAX_LEN];
static int  snake_len;
static int  dir, next_dir;
static pt_t food;
static int  score;
static int  best_score;
static int  game_over;
static int  paused;
static uint32_t last_move_tick;
static uint32_t move_interval;

static uint8_t sk_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

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

static int is_snake_cell(int x, int y)
{
    for (int i = 0; i < snake_len; i++)
        if (snake[i].x == x && snake[i].y == y) return 1;
    return 0;
}

static void place_food(void)
{
    /* MAX_LEN cells total, board is never more than ~2/3 full in
     * practice (snake filling the whole board would already be a win),
     * so a plain rejection loop is fine — no need for a free-list. */
    do {
        food.x = rand() % BOARD_COLS;
        food.y = rand() % BOARD_ROWS;
    } while (is_snake_cell(food.x, food.y));
}

static void reset_game(void)
{
    snake_len = 3;
    dir = next_dir = DIR_RIGHT;
    snake[0].x = BOARD_COLS / 2;     snake[0].y = BOARD_ROWS / 2;
    snake[1].x = snake[0].x - 1;     snake[1].y = snake[0].y;
    snake[2].x = snake[0].x - 2;     snake[2].y = snake[0].y;
    score = 0;
    game_over = 0;
    paused = 0;
    move_interval = 15; /* ticks between moves, ~150ms at 100Hz; speeds up as score rises */
    place_food();
    last_move_tick = task_get_ticks();
}

static void draw_border(void)
{
    uint8_t border = sk_color(VGA_LIGHT_GREEN, VGA_BLACK);
    for (int x = -1; x <= BOARD_COLS; x++) {
        put_char_at(BOARD_X0 + x, BOARD_Y0 - 1, '-', border);
        put_char_at(BOARD_X0 + x, BOARD_Y0 + BOARD_ROWS, '-', border);
    }
    for (int y = -1; y <= BOARD_ROWS; y++) {
        put_char_at(BOARD_X0 - 1, BOARD_Y0 + y, '|', border);
        put_char_at(BOARD_X0 + BOARD_COLS, BOARD_Y0 + y, '|', border);
    }
}

static void draw_status(void)
{
    char buf[96];
    int k = 0;
    const char *p = "Score: ";
    while (*p) buf[k++] = *p++;
    k += fmt_uint(buf + k, (unsigned int)score);
    p = "   Best: ";
    while (*p) buf[k++] = *p++;
    k += fmt_uint(buf + k, (unsigned int)best_score);
    p = "   Arrows to move, P to pause";
    while (*p) buf[k++] = *p++;
    if (game_over) {
        p = "   *** GAME OVER - press Enter to restart ***";
        while (*p) buf[k++] = *p++;
    } else if (paused) {
        p = "   *** PAUSED ***";
        while (*p) buf[k++] = *p++;
    }
    while (k < 78) buf[k++] = ' ';
    buf[k] = 0;
    put_str(0, STATUS_ROW, buf, sk_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void draw_game(int full)
{
    if (full) {
        uint8_t bg = sk_color(VGA_LIGHT_GREY, VGA_BLACK);
        for (int y = 0; y < BOARD_ROWS; y++)
            for (int x = 0; x < BOARD_COLS; x++)
                put_char_at(BOARD_X0 + x, BOARD_Y0 + y, ' ', bg);
        draw_border();
    }

    uint8_t head_c = sk_color(VGA_WHITE, VGA_GREEN);
    uint8_t body_c = sk_color(VGA_BLACK, VGA_GREEN);
    for (int i = 0; i < snake_len; i++) {
        char ch = (i == 0) ? '@' : 'o';
        put_char_at(BOARD_X0 + snake[i].x, BOARD_Y0 + snake[i].y, ch, (i == 0) ? head_c : body_c);
    }
    put_char_at(BOARD_X0 + food.x, BOARD_Y0 + food.y, '*', sk_color(VGA_LIGHT_RED, VGA_BLACK));

    draw_status();
    terminal_setcolor(sk_color(VGA_LIGHT_GREY, VGA_BLACK));
}

static void erase_cell(pt_t p)
{
    put_char_at(BOARD_X0 + p.x, BOARD_Y0 + p.y, ' ', sk_color(VGA_LIGHT_GREY, VGA_BLACK));
}

static void step_game(void)
{
    dir = next_dir;

    int dx = 0, dy = 0;
    switch (dir) {
        case DIR_LEFT:  dx = -1; break;
        case DIR_RIGHT: dx = 1;  break;
        case DIR_UP:    dy = -1; break;
        case DIR_DOWN:  dy = 1;  break;
    }

    pt_t new_head = { snake[0].x + dx, snake[0].y + dy };

    if (new_head.x < 0 || new_head.x >= BOARD_COLS ||
        new_head.y < 0 || new_head.y >= BOARD_ROWS ||
        is_snake_cell(new_head.x, new_head.y)) {
        game_over = 1;
        if (score > best_score) best_score = score;
        draw_status();
        return;
    }

    int ate = (new_head.x == food.x && new_head.y == food.y);
    pt_t tail = snake[snake_len - 1];

    for (int i = snake_len; i > 0; i--) snake[i] = snake[i - 1];
    snake[0] = new_head;
    if (ate) {
        if (snake_len < MAX_LEN - 1) snake_len++;
        score += 10;
        if (move_interval > 5) move_interval--; /* speed up gradually */
        place_food();
    } else {
        erase_cell(tail);
    }

    uint8_t head_c = sk_color(VGA_WHITE, VGA_GREEN);
    uint8_t body_c = sk_color(VGA_BLACK, VGA_GREEN);
    if (snake_len > 1)
        put_char_at(BOARD_X0 + snake[1].x, BOARD_Y0 + snake[1].y, 'o', body_c);
    put_char_at(BOARD_X0 + snake[0].x, BOARD_Y0 + snake[0].y, '@', head_c);
    put_char_at(BOARD_X0 + food.x, BOARD_Y0 + food.y, '*', sk_color(VGA_LIGHT_RED, VGA_BLACK));
    draw_status();
    terminal_setcolor(sk_color(VGA_LIGHT_GREY, VGA_BLACK));
}

void snake_run(void)
{
    srand(task_get_ticks() ^ 0x5A5A5A5A);
    best_score = 0;
    reset_game();
    terminal_clear();
    draw_game(1);

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int spec = keyboard_get_special();
        if (spec == 1 && dir != DIR_RIGHT) next_dir = DIR_LEFT;
        else if (spec == 2 && dir != DIR_LEFT) next_dir = DIR_RIGHT;
        else if (spec == 3 && dir != DIR_DOWN) next_dir = DIR_UP;
        else if (spec == 4 && dir != DIR_UP) next_dir = DIR_DOWN;

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if ((c == 'p' || c == 'P') && !game_over) {
                paused = !paused;
                draw_status();
            } else if (c == '\n' && game_over) {
                reset_game();
                draw_game(1);
            }
        }

        if (!game_over && !paused) {
            uint32_t now = task_get_ticks();
            if (now - last_move_tick >= move_interval) {
                last_move_tick = now;
                step_game();
            }
        }

        task_yield();
    }
}
