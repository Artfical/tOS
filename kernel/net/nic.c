#include "nic.h"
#include "rtl8139.h"
#include "pcnet.h"
#include "e1000.h"
#include "virtio_net.h"
#include "ne2000.h"
#include "net.h"
#include "serial.h"
#include "terminal.h"
#include "string.h"

void (*nic_send)(void *data, int len) = 0;
int  (*nic_poll)(uint8_t *buf, int max_len) = 0;

char     nic_driver_name[32] = "None";
uint32_t nic_rx_packets = 0;
uint32_t nic_tx_packets = 0;
uint32_t nic_rx_bytes   = 0;
uint32_t nic_tx_bytes   = 0;

int nic_init(void)
{
    nic_send = 0;
    nic_poll = 0;

    serial_write("nic: probing RTL8139... ");
    if (rtl8139_init() == 0) {
        nic_send = rtl8139_send;
        nic_poll = rtl8139_poll;
        strncpy(nic_driver_name, "RTL8139", 31);
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing PCnet... ");
    if (pcnet_init() == 0) {
        nic_send = pcnet_send;
        nic_poll = pcnet_poll;
        strncpy(nic_driver_name, "PCnet", 31);
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing E1000... ");
    if (e1000_init() == 0) {
        nic_send = e1000_send;
        nic_poll = e1000_poll;
        strncpy(nic_driver_name, "E1000", 31);
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing virtio-net... ");
    if (virtio_net_init() == 0) {
        nic_send = virtio_net_send;
        nic_poll = virtio_net_poll;
        strncpy(nic_driver_name, "virtio-net", 31);
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\nnic: probing NE2000... ");
    if (ne2000_init() == 0) {
        nic_send = ne2000_send;
        nic_poll = ne2000_poll;
        strncpy(nic_driver_name, "NE2000", 31);
        serial_write("OK\n");
        return 0;
    }
    serial_write("no\n");
    return -1;
}
