#include "usb_wdm.h"
#include "io.h"
#include "klog.h"

int usb_wdm_init(void)
{
    klog_write("usb_wdm: stub init\n");
    return 0;
}

void usb_wdm_shutdown(void)
{
    klog_write("usb_wdm: stub shutdown\n");
}
