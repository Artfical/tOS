#include "gre.h"
#include "net.h"
#include "ip.h"
#include "arp.h"
#include "route.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"
#include "nic.h"

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
    /* A negative length wrapped into a huge malloc()/memcpy(), and anything that
     * does not fit one 1500-byte frame together with the outer IP and GRE
     * headers went out oversized (there is no fragmentation). */
    if (inner_len < 0 || inner_len > 1500 - (int)sizeof(ip_hdr_t) - gre_len) return -1;
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

    nic_transmit(buf, total);       /* pads short frames to the Ethernet minimum, unlike raw nic_send */
    free(buf);
    return 0;
}

/* ones-complement checksum over the whole GRE packet (RFC 2784 2.1); a packet
 * whose stored checksum is right sums to 0xFFFF */
static int gre_checksum_ok(const uint8_t *p, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
}

static int gre_depth;   /* decapsulation nesting, bounded below */

void gre_handle(ip_hdr_t *outer_ip, void *pkt, int len)
{
    if (len < (int)sizeof(gre_hdr_t)) return;
    gre_hdr_t *gre = (gre_hdr_t *)pkt;
    uint16_t flags = ntohs(gre->flags);
    uint16_t proto = ntohs(gre->proto);
    const uint8_t *b = (const uint8_t *)pkt;
    int offset = sizeof(gre_hdr_t);

    /* RFC 2784: version must be 0; the routing bit (source routing) is not
     * supported and such packets must be discarded. */
    if (flags & 0x0007) return;
    if (flags & 0x4000) return;

    int has_ck = (flags & 0x8000) != 0;
    int has_key = (flags & GRE_FLAG_KEY) != 0;
    /* Skip optional checksum+reserved */
    if (has_ck) offset += 4;
    int key_off = offset;
    /* Skip key */
    if (has_key) offset += 4;
    /* Skip seq */
    if (flags & GRE_FLAG_SEQ) offset += 4;

    if (offset >= len) return;

    /* Only decapsulate for a tunnel that was configured: it must be this
     * host's local end, come from its remote end and use the same key (or none).
     * Anyone on the network could previously wrap a packet with a forged
     * source address in GRE and have it injected into the stack. */
    uint32_t key = 0;
    if (has_key) key = ((uint32_t)b[key_off] << 24) | ((uint32_t)b[key_off + 1] << 16) |
                       ((uint32_t)b[key_off + 2] << 8) | b[key_off + 3];
    int matched = 0;
    for (int i = 0; i < GRE_TUNNEL_MAX; i++) {
        gre_tunnel_t *t = &tunnels[i];
        if (!t->valid || t->remote_ip != outer_ip->src_ip || t->local_ip != outer_ip->dst_ip) continue;
        if (t->use_key ? (has_key && key == t->key) : !has_key) { matched = 1; break; }
    }
    if (!matched) return;
    if (has_ck && !gre_checksum_ok(b, len)) return;
    if (gre_depth >= 2) return;                     /* GRE inside GRE inside GRE ...: stop */

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
        gre_depth++;
        ip_handle((uint8_t *)pkt + offset, len - offset);
        gre_depth--;
    }
}
