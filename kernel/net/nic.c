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
#include "klog.h"

uint32_t nic_rx_packets = 0;
uint32_t nic_tx_packets = 0;
uint32_t nic_rx_bytes   = 0;
uint32_t nic_tx_bytes   = 0;

void (*nic_send)(void *data, int len) = 0;
int  (*nic_poll)(uint8_t *buf, int max_len) = 0;

/* The statistics used to be bumped by whichever caller remembered to: net_poll()
 * counted received frames, ip_send() and dhcp counted some sent ones, and
 * everything else (TCP, SCTP, DCCP, ARP, DNS replies, tunnels ...) polled and
 * transmitted without counting, so 'ifconfig' showed 0 received packets after a
 * 27 KB download. The driver functions are now wrapped once, here. */
static void (*drv_send)(void *data, int len);
static int  (*drv_poll)(uint8_t *buf, int max_len);

static void counted_send(void *data, int len)
{
    if (!drv_send) return;
    nic_tx_packets++;
    nic_tx_bytes += (uint32_t)len;
    drv_send(data, len);
}

static int counted_poll(uint8_t *buf, int max_len)
{
    if (!drv_poll) return 0;
    int n = drv_poll(buf, max_len);
    if (n > 0) { nic_rx_packets++; nic_rx_bytes += (uint32_t)n; }
    return n;
}

static void nic_attach(void (*send)(void *, int), int (*poll)(uint8_t *, int))
{
    drv_send = send;
    drv_poll = poll;
    nic_send = counted_send;
    nic_poll = counted_poll;
}

char     nic_driver_name[32] = "None";

/* Probing status previously only went to serial_write() -- visible on
 * a real serial console, but invisible to `dmesg` (which reads back
 * klog's ring buffer, see klog_write()) since nothing here ever wrote
 * to it. That made it impossible to tell, from inside a running VM
 * with no serial console attached, whether NIC probing found nothing
 * at all versus found a card that just isn't getting ARP replies --
 * two very different problems that look identical from the shell
 * ("no route to host"). Mirror every probing line to klog_write() too,
 * the same way kernel/drivers/audio/ich.c already does for its own
 * unsupported-device diagnostics. */
static void nic_log(const char *s)
{
    serial_write(s);
    klog_write(s);
}

#define ETH_MIN_FRAME 60 /* IEEE 802.3 minimum, excluding the 4-byte FCS the hardware appends */

void nic_transmit(void *data, int len)
{
    if (!nic_send) return;
    if (len >= ETH_MIN_FRAME) { nic_send(data, len); return; }
    uint8_t buf[ETH_MIN_FRAME];
    memcpy(buf, data, (size_t)len);
    memset(buf + len, 0, (size_t)(ETH_MIN_FRAME - len));
    nic_send(buf, ETH_MIN_FRAME);
}

int nic_init(void)
{
    nic_send = 0;
    nic_poll = 0;
    drv_send = 0;
    drv_poll = 0;

    nic_log("nic: probing RTL8139... ");
    if (rtl8139_init() == 0) {
        nic_attach(rtl8139_send, rtl8139_poll);
        strncpy(nic_driver_name, "RTL8139", 31);
        nic_log("OK\n");
        return 0;
    }
    nic_log("no\nnic: probing PCnet... ");
    if (pcnet_init() == 0) {
        nic_attach(pcnet_send, pcnet_poll);
        strncpy(nic_driver_name, "PCnet", 31);
        nic_log("OK\n");
        return 0;
    }
    nic_log("no\nnic: probing E1000... ");
    if (e1000_init() == 0) {
        nic_attach(e1000_send, e1000_poll);
        strncpy(nic_driver_name, "E1000", 31);
        nic_log("OK\n");
        return 0;
    }
    nic_log("no\nnic: probing virtio-net... ");
    if (virtio_net_init() == 0) {
        nic_attach(virtio_net_send, virtio_net_poll);
        strncpy(nic_driver_name, "virtio-net", 31);
        nic_log("OK\n");
        return 0;
    }
    nic_log("no\nnic: probing NE2000... ");
    if (ne2000_init() == 0) {
        nic_attach(ne2000_send, ne2000_poll);
        strncpy(nic_driver_name, "NE2000", 31);
        nic_log("OK\n");
        return 0;
    }
    nic_log("no\nnic: no supported network card found -- networking will not work\n");
    return -1;
}
