#include "keyboard.h"
#include "usb_keyboard.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "terminal.h"
#include "gui.h"
#include "wm.h"
#include "net.h"

static volatile char key_buffer[256];
static volatile int key_buffer_head = 0;
static volatile int key_buffer_tail = 0;
static volatile int special_buf[16];
static volatile int special_head = 0;
static volatile int special_tail = 0;
static int caps_lock = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int extended = 0;
volatile int interrupt_char = -1;
void (*interrupt_callback)(void) = NULL;
static int keyboard_layout = KBD_LAYOUT_US;

static const char scancode_lower_us[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_upper_us[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_lower_trq[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '*', '-', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 0xFD, 'o', 'p', 0xF0, 0xFC, '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xFE, 'i', ',',
    0, ';', 'z', 'x', 'c', 'v', 'b', 'n', 'm', 0xF6, 0xE7, '.', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_upper_trq[] = {
    0, 0, '!', '"', '^', '+', '%', '&', '/', '(', ')', '=', '?', '_', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xD0, 0xDC, '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xDE, 0xDD, ';',
    0, ':', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 0xD6, 0xC7, '.', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char *scancode_lower;
static const char *scancode_upper;

void keyboard_set_layout(int layout)
{
    keyboard_layout = layout;
    if (layout == KBD_LAYOUT_TRQ) {
        scancode_lower = scancode_lower_trq;
        scancode_upper = scancode_upper_trq;
    } else {
        scancode_lower = scancode_lower_us;
        scancode_upper = scancode_upper_us;
    }
}

int keyboard_get_layout(void)
{
    return keyboard_layout;
}

void keyboard_push_char(char c)
{
    int next = (key_buffer_head + 1) % 256;
    if (next != key_buffer_tail) {
        key_buffer[key_buffer_head] = c;
        key_buffer_head = next;
    }
}

static void push_special(uint8_t scancode)
{
    int next = (special_head + 1) % 16;
    if (next != special_tail) {
        if (scancode == 0x4B) special_buf[special_head] = 1;
        else if (scancode == 0x4D) special_buf[special_head] = 2;
        else if (scancode == 0x48) special_buf[special_head] = 3;
        else if (scancode == 0x50) special_buf[special_head] = 4;
        special_head = next;
    }
}

static void process_scancode(uint8_t scancode)
{
    if (scancode == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        if (scancode == 0x53) { keyboard_push_char(127); return; }
        if (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D) {
            push_special(scancode);
        }
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }
    if (scancode == 0x1D) { ctrl_pressed = 1; return; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return; }
    if (scancode == 0x3A) { caps_lock = !caps_lock; return; }
    if (scancode & 0x80) return;

    if (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D) {
        push_special(scancode);
        return;
    }

    char key = 0;
    if (scancode < 0x53) {
        if (shift_pressed)
            key = scancode_upper[scancode];
        else
            key = scancode_lower[scancode];

        if (key >= 'a' && key <= 'z' && caps_lock && !ctrl_pressed) key -= 32;
        else if (key >= 'A' && key <= 'Z' && caps_lock && !ctrl_pressed) key += 32;
        if (ctrl_pressed && key >= 'a' && key <= 'z') key -= 0x60;
        else if (ctrl_pressed && key >= 'A' && key <= 'Z') key -= 0x40;
    }
    if (key) {
        keyboard_push_char(key);
        if (interrupt_char >= 0 && key == (char)interrupt_char && interrupt_callback)
            interrupt_callback();
    }
}

static void keyboard_callback(registers_t *regs)
{
    (void)regs;
    asm volatile("cli");
    uint8_t status;
    while (((status = inb(0x64)) & 1) && !(status & 0x20))
        process_scancode(inb(0x60));
    asm volatile("sti");
}

static void keyboard_poll(void)
{
    asm volatile("cli");
    uint8_t status = inb(0x64);
    if ((status & 1) && !(status & 0x20)) process_scancode(inb(0x60));
    asm volatile("sti");
}

int keyboard_data_available(void)
{
    return key_buffer_head != key_buffer_tail;
}

char keyboard_getchar(void)
{
    for (;;) {
        if (wm_current_task_has_focus() && key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % 256;
            return c;
        }
        keyboard_poll();
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
            net_poll();
            asm volatile("hlt");
        }
    }
}

void keyboard_readline(char *buf, int max)
{
    int i = 0;
    for (;;) {
        gui_poll();
        char c = keyboard_getchar();
        if (c == '\n') {
            terminal_putchar('\n');
            buf[i] = '\0';
            return;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 127) {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 3) {
            terminal_putchar('^');
            terminal_putchar('C');
            terminal_putchar('\n');
            buf[0] = '\0';
            return;
        } else if ((unsigned char)c >= ' ' && i < max - 1) {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
}

int keyboard_get_special(void)
{
    if (wm_current_task_has_focus() && special_head != special_tail) {
        int k = special_buf[special_tail];
        special_tail = (special_tail + 1) % 16;
        return k;
    }
    return 0;
}

int keyboard_yesno(void)
{
    for (;;) {
        gui_poll();
        keyboard_poll();
        if (special_head != special_tail) {
            int k = special_buf[special_tail];
            special_tail = (special_tail + 1) % 16;
            if (k == 1) return 0;
            if (k == 2) return 1;
        }
        if (key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % 256;
            if (c == 'y' || c == 'Y') return 1;
            if (c == 'n' || c == 'N') return 0;
            if (c == '\n') return 2;
        }
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
            net_poll();
            asm volatile("hlt");
        }
    }
}

void keyboard_init(void)
{
    keyboard_set_layout(KBD_LAYOUT_US);
    isr_register_handler(33, keyboard_callback);
    irq_ack(1);
    usb_keyboard_init();
}
