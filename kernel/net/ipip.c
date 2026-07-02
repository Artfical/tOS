#include "ipip.h"
#include "net.h"
#include "arp.h"
#include "route.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

static ipip_tunnel_t tunnels[IPIP_MAX];

void ipip_init(void)
{
    memset(tunnels, 0, sizeof(tunnels));
}

int ipip_tunnel_add(uint32_t local_ip, uint32_t remote_ip)
{
    for (int i = 0; i < IPIP_MAX; i++) {
        if (!tunnels[i].valid) {
            tunnels[i].local_ip  = local_ip;
            tunnels[i].remote_ip = remote_ip;
            tunnels[i].valid     = 1;
            return i;
        }
    }
    return -1;
}

int ipip_tunnel_del(int idx)
{
    if (idx < 0 || idx >= IPIP_MAX || !tunnels[idx].valid) return -1;
    tunnels[idx].valid = 0;
    return 0;
}

static void print_ip(uint32_t ip)
{
    uint8_t b[4];
    b[0]=ip&0xFF;b[1]=(ip>>8)&0xFF;b[2]=(ip>>16)&0xFF;b[3]=(ip>>24)&0xFF;
    char buf[16];int i=0;
    for(int n=0;n<4;n++){uint8_t v=b[n];if(v>=100)buf[i++]='0'+v/100;if(v>=10)buf[i++]='0'+(v/10)%10;buf[i++]='0'+v%10;if(n<3)buf[i++]='.';}
    buf[i]='\0';terminal_writestring(buf);
}

void ipip_tunnel_list(void)
{
    terminal_writestring("IP-in-IP tunnels:\n");
    for (int i = 0; i < IPIP_MAX; i++) {
        ipip_tunnel_t *t = &tunnels[i];
        if (!t->valid) continue;
        terminal_writestring("  ["); terminal_putchar('0'+i); terminal_writestring("] ");
        terminal_writestring("local="); print_ip(t->local_ip);
        terminal_writestring(" remote="); print_ip(t->remote_ip);
        terminal_putchar('\n');
    }
}

int ipip_send(int idx, const void *inner_ip, int inner_len)
{
    if (idx < 0 || idx >= IPIP_MAX || !tunnels[idx].valid) return -1;
    ipip_tunnel_t *t = &tunnels[idx];

    int eth_len = 14;
    int total   = eth_len + (int)sizeof(ip_hdr_t) + inner_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    uint8_t mac[6];
    uint32_t nh = route_lookup(t->local_ip, t->remote_ip);
    if (!nh) nh = t->remote_ip;
    if (arp_resolve(nh, mac) != 0) { free(buf); return -1; }

    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)(buf + eth_len);
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + inner_len));
    ip->ttl       = 64;
    ip->protocol  = IPPROTO_IPIP;
    ip->src_ip    = t->local_ip;
    ip->dst_ip    = t->remote_ip;
    uint32_t sum = 0;
    uint8_t *h = (uint8_t *)ip;
    for (int i = 0; i < 20; i += 2) sum += ((uint16_t)h[i] << 8) | h[i+1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->checksum = htons((uint16_t)(~sum & 0xFFFF));

    memcpy(buf + eth_len + sizeof(ip_hdr_t), inner_ip, inner_len);

    extern void (*nic_send)(void *, int);
    if (nic_send) nic_send(buf, total);
    free(buf);
    return 0;
}

void ipip_handle(ip_hdr_t *outer_ip, void *pkt, int len)
{
    terminal_writestring("[IPIP] decap from ");
    print_ip(outer_ip->src_ip);
    terminal_putchar('\n');
    /* Re-inject inner IP packet */
    extern void ip_handle(uint8_t *data, int len);
    ip_handle((uint8_t *)pkt, len);
}
