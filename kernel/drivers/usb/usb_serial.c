#include "usb_serial.h"
#include "io.h"
#include "klog.h"

int usb_serial_init(void)
{
    klog_write("usb_serial: stub init\n");
    return 0;
}

void usb_serial_shutdown(void)
{
    klog_write("usb_serial: stub shutdown\n");
}
