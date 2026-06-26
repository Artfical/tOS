#include "synaptics.h"
#include "io.h"
#include "klog.h"

#define AUX_DATA 0x60
#define AUX_STATUS 0x64

static void aux_wait_write(void)
{
    int t = 100000;
    while (t-- && (inb(AUX_STATUS) & 2));
}

static void aux_wait_read(void)
{
    int t = 100000;
    while (t-- && !(inb(AUX_STATUS) & 1));
}

static void aux_write(uint8_t val)
{
    aux_wait_write();
    outb(AUX_STATUS, 0xD4);
    aux_wait_write();
    outb(AUX_DATA, val);
}

static uint8_t aux_read(void)
{
    aux_wait_read();
    return inb(AUX_DATA);
}

/* "Sliced command" knock: PS/2 mice ignore unknown commands, but Synaptics
   pads use this Set-Scaling/Set-Resolution sequence to smuggle an extra
   command byte to the touchpad firmware, 2 bits at a time. */
static void sliced_command(uint8_t command)
{
    aux_write(0xE6); aux_read();
    for (int shift = 6; shift >= 0; shift -= 2) {
        uint8_t bits = (command >> shift) & 0x03;
        aux_write(0xE8); aux_read();
        aux_write(bits); aux_read();
    }
}

static void status_request(uint8_t out[3])
{
    aux_write(0xE9); aux_read();
    out[0] = aux_read();
    out[1] = aux_read();
    out[2] = aux_read();
}

int synaptics_detect(void)
{
    sliced_command(0x00); /* SYN_QUE_IDENTIFY */
    uint8_t r[3];
    status_request(r);
    return r[1] == 0x47;
}

int synaptics_init(void)
{
    if (!synaptics_detect()) return -1;
    klog_write("synaptics: TouchPad detected, using standard PS/2 relative mode\n");
    return 0;
}

void synaptics_shutdown(void)
{
    klog_write("synaptics: shutdown\n");
}
