#include "usb_mouse.h"
static int usb_mouse_found = 0;
void usb_mouse_init(void)
{
    usb_mouse_found = 0;
}
int usb_mouse_read(int *dx, int *dy, uint8_t *buttons)
{
    (void)dx;
    (void)dy;
    (void)buttons;
    return 0;
}
int usb_mouse_available(void)
{
    return usb_mouse_found;
}
