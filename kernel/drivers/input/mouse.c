#include "mouse.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "terminal.h"

#define MOUSE_IRQ 12
#define MOUSE_PORT 0x60
#define STATUS_PORT 0x64

static volatile int mouse_x = 40;
static volatile int mouse_y = 12;
static volatile uint8_t mouse_buttons = 0;
static volatile int mouse_click_pending = 0;
static volatile int click_x = 0;
static volatile int click_y = 0;

static void mouse_wait(int type)
{
    int t = 100000;
    if (type == 0) {
        while (t-- && (inb(STATUS_PORT) & 2));
    } else {
        while (t-- && !(inb(STATUS_PORT) & 1));
    }
}

static void mouse_write(uint8_t val)
{
    mouse_wait(0);
    outb(STATUS_PORT, 0xD4);
    mouse_wait(0);
    outb(MOUSE_PORT, val);
}

static uint8_t mouse_read(void)
{
    mouse_wait(1);
    return inb(MOUSE_PORT);
}

static int mouse_packet_phase = 0;
static uint8_t mouse_packet[3];

static void mouse_callback(registers_t *regs)
{
    (void)regs;
    uint8_t status = inb(STATUS_PORT);
    if (!(status & 1)) return;
    if (!(status & 0x20)) return;

    uint8_t data = inb(MOUSE_PORT);

    switch (mouse_packet_phase) {
        case 0:
            if (!(data & 0x08)) return;
            mouse_packet[0] = data;
            mouse_packet_phase = 1;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_packet_phase = 2;
            break;
        case 2:
            mouse_packet[2] = data;
            mouse_packet_phase = 0;

            int dx = (int)(int8_t)mouse_packet[1];
            int dy = -(int)(int8_t)mouse_packet[2];

            int nx = mouse_x + dx;
            if (nx < 0) nx = 0;
            if (nx >= 80) nx = 79;
            mouse_x = nx;

            int ny = mouse_y + dy;
            if (ny < 0) ny = 0;
            if (ny >= 25) ny = 24;
            mouse_y = ny;

            mouse_buttons = mouse_packet[0] & 0x07;

            if ((mouse_packet[0] & 1) && !mouse_click_pending) {
                mouse_click_pending = 1;
                click_x = mouse_x;
                click_y = mouse_y;
            }
            break;
    }
}

static void mouse_enable_irq12(void)
{
    mouse_wait(0);
    outb(STATUS_PORT, 0x20);
    mouse_wait(1);
    uint8_t config = inb(MOUSE_PORT);

    config |= 0x02;

    mouse_wait(0);
    outb(STATUS_PORT, 0x60);
    mouse_wait(0);
    outb(MOUSE_PORT, config);
}

void mouse_init(void)
{
    mouse_wait(0);
    outb(STATUS_PORT, 0xA8);
    mouse_wait(0);

    mouse_write(0xF6);
    mouse_read();

    mouse_enable_irq12();

    mouse_write(0xF4);
    mouse_read();

    isr_register_handler(32 + MOUSE_IRQ, mouse_callback);
    irq_ack(MOUSE_IRQ);
}

void mouse_get_state(int *x, int *y, uint8_t *buttons)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
}

int mouse_clicked(void)
{
    return mouse_click_pending;
}

int mouse_get_click(int *x, int *y)
{
    if (!mouse_click_pending) return 0;
    if (x) *x = click_x;
    if (y) *y = click_y;
    mouse_click_pending = 0;
    return 1;
}
