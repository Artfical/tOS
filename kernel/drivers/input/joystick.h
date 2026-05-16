#ifndef JOYSTICK_H
#define JOYSTICK_H
#include <stdint.h>
#define JOYSTICK_PORT 0x201
typedef struct {
    int present;
    int x1, y1;
    int x2, y2;
    uint8_t buttons;
} joystick_device_t;
int joystick_init(joystick_device_t *dev);
int joystick_read(joystick_device_t *dev);
#endif
