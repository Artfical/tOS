#include "usb_cdc_ether.h"
#include "io.h"
#include "klog.h"

int usb_cdc_ether_init(void)
{
    klog_write("usb_cdc_ether: stub init\n");
    return 0;
}

void usb_cdc_ether_shutdown(void)
{
    klog_write("usb_cdc_ether: stub shutdown\n");
}
