#include "usb_gamepad.h"
#include "io.h"
#include "klog.h"

int usb_gamepad_init(void)
{
    klog_write("usb_gamepad: stub init\n");
    return 0;
}

void usb_gamepad_shutdown(void)
{
    klog_write("usb_gamepad: stub shutdown\n");
}
