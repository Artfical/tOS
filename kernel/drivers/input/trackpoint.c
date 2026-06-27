#include "trackpoint.h"
#include "io.h"
#include "klog.h"

#define AUX_DATA 0x60
#define AUX_STATUS 0x64

#define TP_MAGIC_IDENT 0x01

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

/* IBM/Lenovo TrackPoint pointing sticks answer the "Pivot extended ID"
   command (0xE1) with a fixed magic byte followed by a firmware ID. */
int trackpoint_detect(void)
{
    aux_write(0xE1); aux_read();
    uint8_t magic = aux_read();
    aux_read(); /* firmware id, unused */
    return magic == TP_MAGIC_IDENT;
}

int trackpoint_init(void)
{
    if (!trackpoint_detect()) return -1;
    klog_write("trackpoint: TrackPoint detected, using standard PS/2 relative mode\n");
    return 0;
}

void trackpoint_shutdown(void)
{
    klog_write("trackpoint: shutdown\n");
}
