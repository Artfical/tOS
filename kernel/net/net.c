#include "net.h"
#include "nic.h"
#include "arp.h"
#include "ip.h"
#include "ip6.h"
#include "vlan.h"
#include "bridge.h"
#include "ipx.h"
#include "route.h"
#include "fw.h"
#include "gre.h"
#include "ipip.h"
#include "wgtun.h"
#include "dhcp.h"
#include "string.h"
#include "terminal.h"
#include "klog.h"

uint8_t  net_mac[6];
/* QEMU user-mode networking (slirp) defaults -- used as-is if DHCP
 * doesn't find a server (e.g. an isolated test network), overwritten
 * by dhcp_configure() on any real network, where a hardcoded subnet
 * is almost certainly wrong (see route_init()). */
uint32_t net_ip = IP4(10,0,2,15);
uint32_t net_gateway = IP4(10,0,2,2);
uint32_t net_dns = IP4(10,0,2,3);
uint32_t net_netmask = IP4(255,255,255,0);

void net_init(void)
{
    memset(net_mac, 0, 6);
    arp_init();
    if (nic_init() != 0) {
        terminal_writestring("[WARN] No network card found\n");
        return;
    }
    int drc = dhcp_configure();
    if (drc == 0) {
        klog_write("dhcp: lease acquired\n");
    } else {
        klog_write("dhcp: ");
        klog_write(dhcp_strerror(drc));
        klog_write(" -- falling back to compiled-in defaults\n");
    }
    ip6_init(net_mac);
    route_init();
    fw_init();
    gre_init();
    ipip_init();
    wgtun_init();
    terminal_writestring("[OK] Network stack ready\n");
}

/* one received frame handled: 1, or 0 when none was waiting */
/* self-tests only: this many received frames in a thousand are thrown away, to exercise TCP's recovery */
volatile int net_test_loss_permille;
static uint32_t net_test_rng = 12345;

int net_poll(void)
{
    if (!nic_poll) return 0;
    uint8_t buf[1540]; /* 1536 + 4 bytes headroom for VLAN tag */
    int len = nic_poll(buf, 1536);
    if (len <= 0) return 0;
    if (net_test_loss_permille) {
        net_test_rng = net_test_rng * 1103515245u + 12345u;
        if ((net_test_rng >> 16) % 1000u < (uint32_t)net_test_loss_permille) return 1;
    }

    /* Pass through bridge ingress first */
    if (bridge_rx("eth0", buf, len)) return 1;

    /* Strip 802.1Q VLAN tag if present */
    uint16_t vid = 0;
    if (len >= 14) {
        uint16_t etype = (uint16_t)((buf[12] << 8) | buf[13]);
        if (etype == ETHERTYPE_VLAN) {
            vlan_strip(buf, &len, &vid);
            if (!vlan_allowed(vid)) return 1;
        }
    }

    eth_hdr_t *eth = (eth_hdr_t *)buf;
    uint16_t type = ntohs(eth->type);

    if (type == ETHERTYPE_ARP)
        arp_handle(buf, len);
    else if (type == ETHERTYPE_IP)
        ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
    else if (type == ETHERTYPE_IPV6)
        ip6_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
    else if (type == ETHERTYPE_IPX)
        ipx_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
    return 1;
}
