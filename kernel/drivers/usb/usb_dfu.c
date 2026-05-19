#include "usb_dfu.h"
#include "io.h"
#include "klog.h"

int usb_dfu_init(void)
{
    klog_write("usb_dfu: stub init\n");
    return 0;
}

void usb_dfu_shutdown(void)
{
    klog_write("usb_dfu: stub shutdown\n");
}
