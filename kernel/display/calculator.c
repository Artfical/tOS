#include "calculator.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include <stdint.h>

#define CALC_COLS 40
#define CALC_ROWS 20

#define BTN_ROWS 7
#define BTN_COLS 4

static const char *btn_labels[BTN_ROWS][BTN_COLS] = {
    { "C",    "CE",  "%",   "/"   },
    { "7",    "8",   "9",   "*"   },
    { "4",    "5",   "6",   "-"   },
    { "1",    "2",   "3",   "+"   },
    { "0",    ".",   "=",   "="   },
    { "sin",  "cos", "tan", "sqrt"},
    { "log",  "ln",  "x^2", "pi"  },
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
            } else if (r >= 5) {
                btn_color = calc_color(VGA_WHITE, VGA_GREEN);
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

/* No libm in this freestanding kernel, so these are small from-scratch
 * implementations: Newton's method for sqrt/ln, a halve-and-square
 * Taylor series for exp (accurate and fast-converging for any
 * magnitude), and range-reduced Taylor series for sin/cos. Precision is
 * "good enough for a calculator display", not IEEE-grade. */
#define CALC_PI 3.14159265358979323846
#define CALC_LN2 0.69314718055994530942
#define CALC_LN10 2.302585092994046

static double calc_exp(double x)
{
    int neg = 0;
    if (x < 0) { neg = 1; x = -x; }
    int n = 0;
    while (x > 0.5) { x *= 0.5; n++; }
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 25; i++) { term *= x / i; sum += term; }
    for (int i = 0; i < n; i++) sum *= sum;
    return neg ? 1.0 / sum : sum;
}

static double calc_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double r = x;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}

static double calc_ln(double x)
{
    if (x <= 0.0) return 0.0;
    union { double d; uint64_t u; } un;
    un.d = x;
    int exp2 = (int)((un.u >> 52) & 0x7FF) - 1023;
    un.u = (un.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = un.d;
    double y = m - 1.0;
    for (int i = 0; i < 12; i++) y = y - 1.0 + m / calc_exp(y);
    return y + (double)exp2 * CALC_LN2;
}

static double calc_log10(double x)
{
    return calc_ln(x) / CALC_LN10;
}

static double calc_sin(double x)
{
    while (x > CALC_PI) x -= 2 * CALC_PI;
    while (x < -CALC_PI) x += 2 * CALC_PI;
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n <= 10; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

static double calc_cos(double x)
{
    while (x > CALC_PI) x -= 2 * CALC_PI;
    while (x < -CALC_PI) x += 2 * CALC_PI;
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int n = 1; n <= 10; n++) {
        term *= -x2 / ((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
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

    if (strcmp(label, "sin") == 0) { current = calc_sin(current); just_calculated = 1; update_display(); return; }
    if (strcmp(label, "cos") == 0) { current = calc_cos(current); just_calculated = 1; update_display(); return; }
    if (strcmp(label, "tan") == 0) {
        double c = calc_cos(current);
        if (c == 0.0) error_state = 1; else current = calc_sin(current) / c;
        just_calculated = 1;
        update_display();
        return;
    }
    if (strcmp(label, "sqrt") == 0) {
        if (current < 0) error_state = 1; else current = calc_sqrt(current);
        just_calculated = 1;
        update_display();
        return;
    }
    if (strcmp(label, "log") == 0) {
        if (current <= 0) error_state = 1; else current = calc_log10(current);
        just_calculated = 1;
        update_display();
        return;
    }
    if (strcmp(label, "ln") == 0) {
        if (current <= 0) error_state = 1; else current = calc_ln(current);
        just_calculated = 1;
        update_display();
        return;
    }
    if (strcmp(label, "x^2") == 0) { current = current * current; just_calculated = 1; update_display(); return; }
    if (strcmp(label, "pi") == 0) {
        if (just_calculated) just_calculated = 0;
        current = CALC_PI;
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
