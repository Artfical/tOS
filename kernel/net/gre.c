#include "gre.h"
#include "net.h"
#include "ip.h"
#include "arp.h"
#include "route.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

static gre_tunnel_t tunnels[GRE_TUNNEL_MAX];

void gre_init(void)
{
    memset(tunnels, 0, sizeof(tunnels));
}

int gre_tunnel_add(uint32_t local_ip, uint32_t remote_ip,
                   uint32_t key, int use_key)
{
    for (int i = 0; i < GRE_TUNNEL_MAX; i++) {
        if (!tunnels[i].valid) {
            tunnels[i].local_ip  = local_ip;
            tunnels[i].remote_ip = remote_ip;
            tunnels[i].key       = key;
            tunnels[i].use_key   = use_key;
            tunnels[i].valid     = 1;
            return i;
        }
    }
    return -1;
}

int gre_tunnel_del(int idx)
{
    if (idx < 0 || idx >= GRE_TUNNEL_MAX || !tunnels[idx].valid) return -1;
    tunnels[idx].valid = 0;
    return 0;
}

static void print_ip(uint32_t ip)
{
    uint8_t b[4];
    b[0]=ip&0xFF; b[1]=(ip>>8)&0xFF; b[2]=(ip>>16)&0xFF; b[3]=(ip>>24)&0xFF;
    char buf[16]; int i=0;
    for (int n=0;n<4;n++){uint8_t v=b[n];if(v>=100)buf[i++]='0'+v/100;if(v>=10)buf[i++]='0'+(v/10)%10;buf[i++]='0'+v%10;if(n<3)buf[i++]='.';}
    buf[i]='\0'; terminal_writestring(buf);
}

void gre_tunnel_list(void)
{
    terminal_writestring("GRE tunnels:\n");
    for (int i = 0; i < GRE_TUNNEL_MAX; i++) {
        gre_tunnel_t *t = &tunnels[i];
        if (!t->valid) continue;
        terminal_writestring("  [");
        terminal_putchar('0' + i);
        terminal_writestring("] local="); print_ip(t->local_ip);
        terminal_writestring(" remote="); print_ip(t->remote_ip);
        if (t->use_key) {
            terminal_writestring(" key=0x");
            static const char hex[]="0123456789abcdef";
            for (int b=28;b>=0;b-=4) terminal_putchar(hex[(t->key>>b)&0xF]);
        }
        terminal_putchar('\n');
    }
}

int gre_send(int idx, const void *inner_data, int inner_len)
{
    if (idx < 0 || idx >= GRE_TUNNEL_MAX || !tunnels[idx].valid) return -1;
    gre_tunnel_t *t = &tunnels[idx];

    int key_len = t->use_key ? 4 : 0;
    int gre_len = (int)sizeof(gre_hdr_t) + key_len;
    int outer_ip_len = (int)sizeof(ip_hdr_t);
    int eth_len      = 14;
    int total = eth_len + outer_ip_len + gre_len + inner_len;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    /* Ethernet header */
    uint8_t mac[6];
    uint32_t nh = route_lookup(t->local_ip, t->remote_ip);
    if (!nh) nh = t->remote_ip;
    if (arp_resolve(nh, mac) != 0) { free(buf); return -1; }

    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETHERTYPE_IP);

    /* Outer IP header */
    ip_hdr_t *ip = (ip_hdr_t *)(buf + eth_len);
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons((uint16_t)(outer_ip_len + gre_len + inner_len));
    ip->ttl       = 64;
    ip->protocol  = IPPROTO_GRE;
    ip->src_ip    = t->local_ip;
    ip->dst_ip    = t->remote_ip;
    /* checksum */
    uint32_t sum = 0;
    uint8_t *iphb = (uint8_t *)ip;
    for (int i = 0; i < 20; i += 2) sum += ((uint16_t)iphb[i] << 8) | iphb[i+1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->checksum = htons((uint16_t)(~sum & 0xFFFF));

    /* GRE header */
    uint8_t *gp = buf + eth_len + outer_ip_len;
    gre_hdr_t *gre = (gre_hdr_t *)gp;
    gre->flags = htons(t->use_key ? GRE_FLAG_KEY : 0);
    gre->proto = htons(GRE_PROTO_IP);
    if (t->use_key) {
        uint32_t k = htonl(t->key);
        memcpy(gp + sizeof(gre_hdr_t), &k, 4);
    }

    /* Inner payload */
    memcpy(buf + eth_len + outer_ip_len + gre_len, inner_data, inner_len);

    extern void (*nic_send)(void *, int);
    if (nic_send) nic_send(buf, total);
    free(buf);
    return 0;
}

void gre_handle(ip_hdr_t *outer_ip, void *pkt, int len)
{
    if (len < (int)sizeof(gre_hdr_t)) return;
    gre_hdr_t *gre = (gre_hdr_t *)pkt;
    uint16_t flags = ntohs(gre->flags);
    uint16_t proto = ntohs(gre->proto);
    int offset = sizeof(gre_hdr_t);

    /* Skip optional checksum+reserved */
    if (flags & 0x8000) offset += 4;
    /* Skip key */
    if (flags & GRE_FLAG_KEY) offset += 4;
    /* Skip seq */
    if (flags & GRE_FLAG_SEQ) offset += 4;

    if (offset >= len) return;

    terminal_writestring("[GRE] decap from ");
    print_ip(outer_ip->src_ip);
    terminal_writestring(" proto=0x");
    static const char hex[]="0123456789abcdef";
    terminal_putchar(hex[(proto>>12)&0xF]);
    terminal_putchar(hex[(proto>>8)&0xF]);
    terminal_putchar(hex[(proto>>4)&0xF]);
    terminal_putchar(hex[proto&0xF]);
    terminal_putchar('\n');

    if (proto == GRE_PROTO_IP) {
        /* re-inject inner IP packet */
        extern void ip_handle(uint8_t *data, int len);
        ip_handle((uint8_t *)pkt + offset, len - offset);
    }
}
