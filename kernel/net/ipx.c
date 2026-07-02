#include "ipx.h"
#include "net.h"
#include "nic.h"
#include "arp.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

/* Our IPX node address = our MAC; network number = 0 (unconfigured) */
static uint8_t ipx_local_net[4]  = {0, 0, 0, 0};
/* ipx_local_node set from net_mac at first use */

static void print_hex_byte(uint8_t b)
{
    static const char hex[] = "0123456789ABCDEF";
    terminal_putchar(hex[(b >> 4) & 0xF]);
    terminal_putchar(hex[b & 0xF]);
}

static void print_ipx_addr(const uint8_t *net, const uint8_t *node, uint16_t sock)
{
    for (int i = 0; i < 4; i++) print_hex_byte(net[i]);
    terminal_putchar(':');
    for (int i = 0; i < 6; i++) { print_hex_byte(node[i]); if (i < 5) terminal_putchar(':'); }
    terminal_putchar(':');
    print_hex_byte((uint8_t)(sock >> 8));
    print_hex_byte((uint8_t)(sock & 0xFF));
}

/* -----------------------------------------------------------------------
 * Incoming packet handler (called from net_poll when EtherType = 0x8137)
 * ----------------------------------------------------------------------- */
void ipx_handle(void *pkt, int len)
{
    if (len < (int)sizeof(ipx_hdr_t)) return;
    ipx_hdr_t *h = (ipx_hdr_t *)pkt;

    uint16_t total = (uint16_t)((h->length >> 8) | (h->length << 8)); /* big-endian */
    if (total < sizeof(ipx_hdr_t) || total > (uint16_t)len) return;

    terminal_writestring("[IPX] ptype=0x");
    print_hex_byte(h->packet_type);
    terminal_writestring(" src=");
    print_ipx_addr(h->src_net, h->src_node, (uint16_t)((h->src_sock >> 8) | (h->src_sock << 8)));
    terminal_writestring(" dst=");
    print_ipx_addr(h->dst_net, h->dst_node, (uint16_t)((h->dst_sock >> 8) | (h->dst_sock << 8)));
    terminal_writestring(" len=");
    {
        char buf[8]; int i = 7; buf[7] = '\0';
        uint16_t v = total;
        if (v == 0) { buf[6] = '0'; terminal_writestring(buf + 6); }
        else { while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; } terminal_writestring(buf + i); }
    }
    terminal_putchar('\n');

    /* Echo service: reply to IPX_SOCK_ECHO directed at our node */
    int is_broadcast = 1;
    for (int i = 0; i < 6; i++) if (h->dst_node[i] != 0xFF) { is_broadcast = 0; break; }
    int is_ours = (memcmp(h->dst_node, net_mac, 6) == 0);

    if ((is_ours || is_broadcast) &&
        (uint16_t)((h->dst_sock >> 8) | (h->dst_sock << 8)) == IPX_SOCK_ECHO) {
        uint8_t *payload = (uint8_t *)pkt + sizeof(ipx_hdr_t);
        int plen = total - sizeof(ipx_hdr_t);
        ipx_send(h->src_node, h->src_net,
                 (uint16_t)((h->src_sock >> 8) | (h->src_sock << 8)),
                 IPX_SOCK_ECHO, IPX_TYPE_ECHO, payload, plen);
    }
}

/* -----------------------------------------------------------------------
 * Send an IPX packet
 * ----------------------------------------------------------------------- */
int ipx_send(const uint8_t *dst_node, const uint8_t *dst_net,
             uint16_t dst_sock, uint16_t src_sock,
             uint8_t ptype, const void *data, int data_len)
{
    if (data_len < 0) return -1;
    int frame_len = (int)sizeof(eth_hdr_t) + (int)sizeof(ipx_hdr_t) + data_len;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (!frame) return -1;

    /* Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst, dst_node, 6);
    memcpy(eth->src, net_mac,  6);
    eth->type = htons(ETHERTYPE_IPX);

    /* IPX header */
    ipx_hdr_t *h = (ipx_hdr_t *)(frame + sizeof(eth_hdr_t));
    uint16_t total = (uint16_t)(sizeof(ipx_hdr_t) + data_len);
    /* big-endian length */
    h->checksum   = 0xFFFF;
    h->length     = (uint16_t)((total >> 8) | (total << 8));
    h->hop_count  = 0;
    h->packet_type = ptype;
    memcpy(h->dst_net,  dst_net, 4);
    memcpy(h->dst_node, dst_node, 6);
    h->dst_sock   = (uint16_t)((dst_sock >> 8) | (dst_sock << 8));
    memcpy(h->src_net,  ipx_local_net, 4);
    memcpy(h->src_node, net_mac, 6);
    h->src_sock   = (uint16_t)((src_sock >> 8) | (src_sock << 8));

    if (data_len > 0)
        memcpy(frame + sizeof(eth_hdr_t) + sizeof(ipx_hdr_t), data, data_len);

    if (nic_send) nic_send(frame, frame_len);
    free(frame);
    return 0;
}
