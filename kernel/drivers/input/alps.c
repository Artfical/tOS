#include "alps.h"
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

/* ALPS knock: three Set-Scaling-1:1 (0xE6) followed by one Set-Scaling-2:1
   (0xE7), then a Status Request (0xE9). Genuine PS/2 mice just no-op this;
   ALPS touchpads answer with a model signature in bytes 1/2. */
int alps_detect(void)
{
    aux_write(0xE6); aux_read();
    aux_write(0xE6); aux_read();
    aux_write(0xE6); aux_read();
    aux_write(0xE7); aux_read();

    aux_write(0xE9); aux_read();
    uint8_t r0 = aux_read();
    uint8_t r1 = aux_read();
    uint8_t r2 = aux_read();
    (void)r0;

    /* Common ALPS model signatures (byte1, byte2) seen on real hardware.
       Not exhaustive -- ALPS has many protocol sub-versions (V1-V8). */
    static const uint8_t sigs[][2] = {
        {0x33, 0x02}, {0x53, 0x02}, {0x73, 0x02}, {0x73, 0x03},
        {0x20, 0x02}, {0x22, 0x02}, {0x22, 0x0A}, {0x42, 0x02},
    };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (r1 == sigs[i][0] && r2 == sigs[i][1]) return 1;
    }
    return 0;
}

int alps_init(void)
{
    if (!alps_detect()) return -1;
    klog_write("alps: TouchPad detected, using standard PS/2 relative mode\n");
    return 0;
}

void alps_shutdown(void)
{
    klog_write("alps: shutdown\n");
}
