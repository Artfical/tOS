#include "nic.h"
#include "rtl8139.h"
#include "pcnet.h"
#include "e1000.h"
#include "net.h"
#include "serial.h"
#include "terminal.h"
#include "string.h"

void (*nic_send)(void *data, int len) = 0;
int  (*nic_poll)(uint8_t *buf, int max_len) = 0;

int nic_init(void)
{
    nic_send = 0;
    nic_poll = 0;

    serial_write("nic: probing RTL8139... ");
    if (rtl8139_init() == 0) {
        nic_send = rtl8139_send;
        nic_poll = rtl8139_poll;
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing PCnet... ");
    if (pcnet_init() == 0) {
        nic_send = pcnet_send;
        nic_poll = pcnet_poll;
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing E1000... ");
    if (e1000_init() == 0) {
        nic_send = e1000_send;
        nic_poll = e1000_poll;
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\n");
    return -1;
}
