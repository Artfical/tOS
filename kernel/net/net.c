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
#include "string.h"
#include "terminal.h"

uint8_t  net_mac[6];
uint32_t net_ip = IP4(10,0,2,15);
uint32_t net_gateway = IP4(10,0,2,2);
uint32_t net_dns = IP4(10,0,2,3);

void net_init(void)
{
    memset(net_mac, 0, 6);
    arp_init();
    if (nic_init() != 0) {
        terminal_writestring("[WARN] No network card found\n");
        return;
    }
    ip6_init(net_mac);
    route_init();
    fw_init();
    gre_init();
    ipip_init();
    wgtun_init();
    terminal_writestring("[OK] Network stack ready\n");
}

void net_poll(void)
{
    if (!nic_poll) return;
    uint8_t buf[1540]; /* 1536 + 4 bytes headroom for VLAN tag */
    int len = nic_poll(buf, 1536);
    if (len <= 0) return;

    /* Pass through bridge ingress first */
    if (bridge_rx("eth0", buf, len)) return;

    /* Strip 802.1Q VLAN tag if present */
    uint16_t vid = 0;
    if (len >= 14) {
        uint16_t etype = (uint16_t)((buf[12] << 8) | buf[13]);
        if (etype == ETHERTYPE_VLAN) {
            vlan_strip(buf, &len, &vid);
            if (!vlan_allowed(vid)) return;
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
}
