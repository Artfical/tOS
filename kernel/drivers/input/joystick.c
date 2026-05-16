#include "joystick.h"
#include "io.h"
int joystick_init(joystick_device_t *dev)
{
    dev->present = 0;
    dev->x1 = 0;
    dev->y1 = 0;
    dev->x2 = 0;
    dev->y2 = 0;
    dev->buttons = 0;
    uint8_t test = inb(JOYSTICK_PORT);
    if (test != 0xFF) {
        dev->present = 1;
        return 0;
    }
    return -1;
}
int joystick_read(joystick_device_t *dev)
{
    if (!dev->present) return -1;
    outb(JOYSTICK_PORT, 0);
    uint32_t t1 = 0, t2 = 0, t3 = 0, t4 = 0, tb = 0;
    for (int i = 0; i < 100000; i++) {
        uint8_t v = inb(JOYSTICK_PORT);
        if (!t1 && !(v & 1)) t1 = i;
        if (!t2 && !(v & 2)) t2 = i;
        if (!t3 && !(v & 4)) t3 = i;
        if (!t4 && !(v & 8)) t4 = i;
        tb = v & 0xF0;
        if (t1 && t2 && t3 && t4) break;
    }
    dev->x1 = t1;
    dev->y1 = t2;
    dev->x2 = t3;
    dev->y2 = t4;
    dev->buttons = (~tb >> 4) & 0x0F;
    return 0;
}
