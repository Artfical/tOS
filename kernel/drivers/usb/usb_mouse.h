#ifndef USB_MOUSE_H
#define USB_MOUSE_H
#include <stdint.h>
void usb_mouse_init(void);
int usb_mouse_read(int *dx, int *dy, uint8_t *buttons);
int usb_mouse_available(void);
#endif
