#include "usb_printer.h"
#include "io.h"
#include "klog.h"

int usb_printer_init(void)
{
    klog_write("usb_printer: stub init\n");
    return 0;
}

void usb_printer_shutdown(void)
{
    klog_write("usb_printer: stub shutdown\n");
}
