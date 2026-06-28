#include "calculator.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"

#define CALC_COLS 40
#define CALC_ROWS 18

#define BTN_ROWS 5
#define BTN_COLS 4

static const char *btn_labels[BTN_ROWS][BTN_COLS] = {
    { "C",  "CE", "%",  "/"  },
    { "7",  "8",  "9",  "*"  },
    { "4",  "5",  "6",  "-"  },
    { "1",  "2",  "3",  "+"  },
    { "0",  ".",  "=",  "="  },
};

static int btn_x0[BTN_ROWS][BTN_COLS];
static int btn_x1[BTN_ROWS][BTN_COLS];
static int btn_y0[BTN_ROWS][BTN_COLS];
static int btn_y1[BTN_ROWS][BTN_COLS];

static char display[32];
static int display_len;
static double accumulator;
static double current;
static int pending_op;
#define OP_NONE 0
#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4
static int just_calculated;
static int error_state;

static uint8_t calc_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void clear_area(void)
{
    terminal_setcolor(calc_color(VGA_BLACK, VGA_LIGHT_GREY));
    for (int r = 0; r < CALC_ROWS; r++) {
        terminal_setpos(0, (size_t)r);
        for (int c = 0; c < CALC_COLS; c++) terminal_putchar(' ');
    }
}

static void update_display(void)
{
    if (error_state) {
        display_len = 0;
        const char *err = "Error";
        while (err[display_len]) { display[display_len] = err[display_len]; display_len++; }
        display[display_len] = 0;
        return;
    }

    int whole = (int)current;
    double frac = current - (double)whole;

    if (current == 0.0 && !just_calculated) {
        display[0] = '0';
        display[1] = 0;
        display_len = 1;
        return;
    }

    char tmp[24];
    int k = 0;

    if (current < 0) { tmp[k++] = '-'; whole = -whole; frac = -frac; }

    if (whole == 0) {
        tmp[k++] = '0';
    } else {
        char digits[12];
        int nd = 0;
        int v = whole;
        while (v > 0) { digits[nd++] = '0' + (v % 10); v /= 10; }
        for (int i = nd - 1; i >= 0; i--) tmp[k++] = digits[i];
    }

    if (frac > 0.000001 || frac < -0.000001) {
        tmp[k++] = '.';
        for (int i = 0; i < 6 && (frac > 0.000001 || frac < -0.000001); i++) {
            frac *= 10.0;
            int d = (int)frac;
            tmp[k++] = '0' + d;
            frac -= (double)d;
        }
        while (k > 1 && tmp[k - 1] == '0' && tmp[k - 2] != '.') { k--; }
    }

    tmp[k] = 0;

    if (tmp[0] == '-') { display[0] = '-'; k--; }
    for (int i = 0; i < k + (tmp[0] == '-' ? 1 : 0); i++)
        display[i] = tmp[i];
    display[k + (tmp[0] == '-' ? 1 : 0)] = 0;
    display_len = k + (tmp[0] == '-' ? 1 : 0);
}

static void draw_display(void)
{
    terminal_setpos(0, 0);
    terminal_setcolor(calc_color(VGA_LIGHT_GREY, VGA_WHITE));
    for (int i = 0; i < CALC_COLS; i++) terminal_putchar(' ');

    terminal_setpos(0, 1);
    terminal_setcolor(calc_color(VGA_LIGHT_GREY, VGA_WHITE));
    for (int i = 0; i < CALC_COLS; i++) terminal_putchar(' ');

    int x = CALC_COLS - display_len - 2;
    if (x < 1) x = 1;
    put_str(x, 1, display, calc_color(VGA_WHITE, VGA_DARK_GREY));
}

static void draw_buttons(void)
{
    int start_y = 3;
    int btn_w = 8;
    int btn_h = 2;
    int gap_x = 1;
    int gap_y = 0;
    int grid_w = BTN_COLS * (btn_w + gap_x) - gap_x;
    int grid_x = (CALC_COLS - grid_w) / 2;

    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            int bx = grid_x + c * (btn_w + gap_x);
            int by = start_y + r * (btn_h + gap_y);
            btn_x0[r][c] = bx;
            btn_x1[r][c] = bx + btn_w - 1;
            btn_y0[r][c] = by;
            btn_y1[r][c] = by + btn_h - 1;

            uint8_t btn_color;
            const char *label = btn_labels[r][c];

            if (label[0] == '/' || label[0] == '*' || label[0] == '-' || label[0] == '+') {
                btn_color = calc_color(VGA_WHITE, VGA_RED);
            } else if (label[0] == 'C' && label[1] == 'E') {
                btn_color = calc_color(VGA_WHITE, VGA_RED);
            } else if (label[0] == 'C' && label[1] == 0) {
                btn_color = calc_color(VGA_WHITE, VGA_RED);
            } else if (label[0] == '=' ) {
                btn_color = calc_color(VGA_WHITE, VGA_BLUE);
            } else {
                btn_color = calc_color(VGA_BLACK, VGA_WHITE);
            }

            terminal_setpos((size_t)bx, (size_t)by);
            terminal_setcolor(calc_color(VGA_BLACK, VGA_LIGHT_GREY));
            for (int i = 0; i < btn_w; i++) terminal_putchar(' ');

            terminal_setpos((size_t)bx, (size_t)(by + 1));
            terminal_setcolor(calc_color(VGA_BLACK, VGA_LIGHT_GREY));
            for (int i = 0; i < btn_w; i++) terminal_putchar(' ');

            terminal_setpos((size_t)bx, (size_t)by);
            terminal_setcolor(btn_color);
            terminal_putchar('[');

            int lbl_len = (int)strlen(label);
            int pad = (btn_w - 2 - lbl_len) / 2;
            for (int i = 0; i < pad; i++) terminal_putchar(' ');
            for (int i = 0; i < lbl_len; i++) terminal_putchar(label[i]);
            for (int i = 0; i < btn_w - 2 - pad - lbl_len; i++) terminal_putchar(' ');

            terminal_putchar(']');
        }
    }
}

static void redraw(void)
{
    clear_area();
    update_display();
    draw_display();
    draw_buttons();

    put_str(1, CALC_ROWS - 2, "tOS Calculator", calc_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
}

static double parse_number(const char *s, int len)
{
    double result = 0;
    int sign = 1;
    int i = 0;

    if (s[0] == '-') { sign = -1; i = 1; }

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        result = result * 10.0 + (s[i] - '0');
        i++;
    }

    if (i < len && s[i] == '.') {
        i++;
        double frac = 0;
        double place = 0.1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            frac += (s[i] - '0') * place;
            place *= 0.1;
            i++;
        }
        result += frac;
    }

    return result * sign;
}

static void do_operation(double a, double b, int op, double *result)
{
    switch (op) {
        case OP_ADD: *result = a + b; break;
        case OP_SUB: *result = a - b; break;
        case OP_MUL: *result = a * b; break;
        case OP_DIV:
            if (b == 0.0) { error_state = 1; return; }
            *result = a / b;
            break;
        default: *result = b; break;
    }
}

static void on_button(const char *label)
{
    if (error_state && label[0] != 'C') {
        return;
    }

    if (label[0] == 'C' && label[1] == 0) {
        current = 0;
        accumulator = 0;
        pending_op = OP_NONE;
        just_calculated = 0;
        error_state = 0;
        update_display();
        return;
    }

    if (label[0] == 'C' && label[1] == 'E') {
        current = 0;
        just_calculated = 0;
        error_state = 0;
        update_display();
        return;
    }

    if (label[0] >= '0' && label[0] <= '9') {
        if (just_calculated) {
            current = 0;
            just_calculated = 0;
        }
        current = current * 10.0 + (label[0] - '0');
        update_display();
        return;
    }

    if (label[0] == '.') {
        if (just_calculated) {
            current = 0;
            just_calculated = 0;
        }
        char tmp[24];
        int k = 0;
        if (current < 0) { tmp[k++] = '-'; }
        int whole = (int)(current < 0 ? -current : current);
        if (whole == 0) { tmp[k++] = '0'; }
        else {
            char digits[12];
            int nd = 0;
            int v = whole;
            while (v > 0) { digits[nd++] = '0' + (v % 10); v /= 10; }
            for (int i = nd - 1; i >= 0; i--) tmp[k++] = digits[i];
        }
        int already_has_dot = 0;
        for (int i = 0; i < display_len; i++)
            if (display[i] == '.') already_has_dot = 1;
        if (!already_has_dot) {
            tmp[k++] = '.';
            tmp[k] = 0;
            current = parse_number(tmp, k);
            update_display();
        }
        return;
    }

    if (label[0] == '%') {
        current = current / 100.0;
        update_display();
        return;
    }

    if (label[0] == '/' || label[0] == '*' || label[0] == '-' || label[0] == '+') {
        int new_op;
        if (label[0] == '+') new_op = OP_ADD;
        else if (label[0] == '-') new_op = OP_SUB;
        else if (label[0] == '*') new_op = OP_MUL;
        else new_op = OP_DIV;

        if (pending_op != OP_NONE && !just_calculated) {
            do_operation(accumulator, current, pending_op, &accumulator);
        } else {
            accumulator = current;
        }
        pending_op = new_op;
        current = 0;
        just_calculated = 0;
        update_display();
        return;
    }

    if (label[0] == '=') {
        if (pending_op != OP_NONE) {
            do_operation(accumulator, current, pending_op, &current);
            pending_op = OP_NONE;
            accumulator = 0;
            just_calculated = 1;
            update_display();
        }
        return;
    }
}

void calculator_run(void)
{
    current = 0;
    accumulator = 0;
    pending_op = OP_NONE;
    just_calculated = 0;
    error_state = 0;
    display[0] = '0';
    display[1] = 0;
    display_len = 1;

    redraw();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            for (int r = 0; r < BTN_ROWS; r++) {
                for (int c = 0; c < BTN_COLS; c++) {
                    if (ccx >= btn_x0[r][c] && ccx <= btn_x1[r][c] &&
                        ccy >= btn_y0[r][c] && ccy <= btn_y1[r][c]) {
                        on_button(btn_labels[r][c]);
                        redraw();
                        break;
                    }
                }
            }
            continue;
        }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c >= '0' && c <= '9') {
                char buf[2] = { c, 0 };
                on_button(buf);
                redraw();
            } else if (c == '.') {
                on_button(".");
                redraw();
            } else if (c == '+') {
                on_button("+");
                redraw();
            } else if (c == '-') {
                on_button("-");
                redraw();
            } else if (c == '*') {
                on_button("*");
                redraw();
            } else if (c == '/') {
                on_button("/");
                redraw();
            } else if (c == '\n' || c == '=') {
                on_button("=");
                redraw();
            } else if (c == '\b' || c == 127) {
                on_button("CE");
                redraw();
            } else if (c == 0x1B) {
                on_button("C");
                redraw();
            } else if (c == '%') {
                on_button("%");
                redraw();
            }
        }

        task_yield();
    }
}
