#include "usb_bluetooth.h"
#include "io.h"
#include "klog.h"

int usb_bluetooth_init(void)
{
    klog_write("usb_bluetooth: stub init\n");
    return 0;
}

void usb_bluetooth_shutdown(void)
{
    klog_write("usb_bluetooth: stub shutdown\n");
}
