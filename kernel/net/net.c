#include "net.h"
#include "nic.h"
#include "arp.h"
#include "ip.h"
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
    terminal_writestring("[OK] Network stack ready\n");
}

void net_poll(void)
{
    if (!nic_poll) return;
    uint8_t buf[1536];
    int len = nic_poll(buf, sizeof(buf));
    if (len <= 0) return;
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    if (ntohs(eth->type) == ETHERTYPE_ARP)
        arp_handle(buf, len);
    else if (ntohs(eth->type) == ETHERTYPE_IP)
        ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
}
