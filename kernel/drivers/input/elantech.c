#include "elantech.h"
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

/* Elantech knock: four Set-Scaling-1:1 (0xE6) commands followed by a Status
   Request (0xE9). Real PS/2 mice no-op this; Elantech touchpads answer with
   a recognizable hardware-version-1 signature. */
int elantech_detect(void)
{
    aux_write(0xE6); aux_read();
    aux_write(0xE6); aux_read();
    aux_write(0xE6); aux_read();
    aux_write(0xE6); aux_read();

    aux_write(0xE9); aux_read();
    uint8_t r0 = aux_read();
    uint8_t r1 = aux_read();
    uint8_t r2 = aux_read();

    return r0 == 0x3C && r1 == 0x03 && r2 <= 0x02;
}

int elantech_init(void)
{
    if (!elantech_detect()) return -1;
    klog_write("elantech: TouchPad detected, using standard PS/2 relative mode\n");
    return 0;
}

void elantech_shutdown(void)
{
    klog_write("elantech: shutdown\n");
}
