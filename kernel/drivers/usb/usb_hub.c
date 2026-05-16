#include "usb_hub.h"
#include "usb.h"
#include "terminal.h"
int usb_hub_init(void)
{
    terminal_writestring("USB Hub: init\n");
    return 0;
}
int usb_hub_enumerate_ports(void)
{
    return 0;
}
