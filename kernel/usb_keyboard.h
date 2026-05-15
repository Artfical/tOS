#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

void usb_keyboard_init(void);
int usb_keyboard_read(char *c);
int usb_keyboard_available(void);

#endif
