#include "keyboard.h"
#include "usb_keyboard.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "terminal.h"
#include "gui.h"
#include <stdint.h>

static volatile char key_buffer[256];
static volatile int key_buffer_head = 0;
static volatile int key_buffer_tail = 0;
static volatile int special_buf[16];
static volatile int special_head = 0;
static volatile int special_tail = 0;
static int caps_lock = 0;
static int shift_pressed = 0;

void keyboard_push_char(char c)
{
    int next = (key_buffer_head + 1) % 256;
    if (next != key_buffer_tail) {
        key_buffer[key_buffer_head] = c;
        key_buffer_head = next;
    }
}

static const char scancode_ascii_lower[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_ascii_upper[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static void keyboard_callback(registers_t *regs)
{
    (void)regs;
    uint8_t scancode = inb(0x60);

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return;
    }
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return;
    }

    if (scancode & 0x80) return;

    if (scancode == 0x4B || scancode == 0x4D) {
        int next = (special_head + 1) % 16;
        if (next != special_tail) {
            special_buf[special_head] = (scancode == 0x4B) ? 1 : 2;
            special_head = next;
        }
        return;
    }

    char key = 0;
    if (scancode < sizeof(scancode_ascii_lower)) {
        if (shift_pressed)
            key = scancode_ascii_upper[scancode];
        else
            key = scancode_ascii_lower[scancode];

        if (key >= 'a' && key <= 'z' && caps_lock) {
            key -= 32;
        } else if (key >= 'A' && key <= 'Z' && caps_lock) {
            key += 32;
        }
    }

    if (key) {
        keyboard_push_char(key);
    }
}

int keyboard_data_available(void)
{
    return key_buffer_head != key_buffer_tail;
}

char keyboard_getchar(void)
{
    for (;;) {
        if (key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % 256;
            return c;
        }
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
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
        } else if (c >= ' ' && i < max - 1) {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
}

int keyboard_yesno(int timeout_seconds)
{
    uint32_t start = irq_get_tick();
    for (;;) {
        gui_poll();
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
        if (timeout_seconds > 0) {
            uint32_t elapsed = irq_get_tick() - start;
            if (elapsed >= (uint32_t)timeout_seconds * 18) {
                terminal_writestring("\n[Timeout] Auto-selecting GUI mode");
                return 1;
            }
        }
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
            asm volatile("hlt");
        }
    }
}

void keyboard_init(void)
{
    isr_register_handler(33, keyboard_callback);
    irq_ack(1);
    usb_keyboard_init();
}
