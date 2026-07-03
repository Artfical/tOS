#include "dns.h"
#include "net.h"
#include "udp.h"
#include "ip.h"
#include "arp.h"
#include "nic.h"
#include "string.h"
#include "memory.h"
#include "scheduler.h"
#include "debugmon.h"

#define DNS_PORT 53

static void dns_build_name(uint8_t *buf, int *off, const char *name)
{
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.') dot++;
        int len = dot - name;
        buf[(*off)++] = len;
        for (int i = 0; i < len; i++) buf[(*off)++] = name[i];
        name = dot;
        if (*name == '.') name++;
    }
    buf[(*off)++] = 0;
}

int dns_resolve(const char *hostname, uint32_t *ip_out)
{
    uint8_t pkt[256];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = 0xAA; pkt[1] = 0xAA;
    pkt[2] = 0x01; pkt[3] = 0x00;
    pkt[4] = 0x00; pkt[5] = 0x01;
    pkt[6] = 0x00; pkt[7] = 0x00;
    pkt[8] = 0x00; pkt[9] = 0x00;
    pkt[10] = 0x00; pkt[11] = 0x00;

    int off = 12;
    dns_build_name(pkt, &off, hostname);
    pkt[off++] = 0x00; pkt[off++] = 0x01;
    pkt[off++] = 0x00; pkt[off++] = 0x01;

    uint8_t resp[512];
    int rx_port = 12345;
    udp_open(rx_port);

    if (udp_send(net_dns, DNS_PORT, rx_port, pkt, off) != 0)
        return -1;

    /* Wall-clock timeout, not an iteration count — see arp_resolve()
     * for why a fixed retry count is unreliable across drivers. */
    uint32_t deadline = debugmon_uptime_ms() + 3000;
    while (debugmon_uptime_ms() < deadline) {
        uint8_t buf[1536];
        int len = nic_poll(buf, sizeof(buf));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(buf, len);
            else if (ntohs(eth->type) == ETHERTYPE_IP)
                ip_handle(buf + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
        }

        uint32_t src_ip;
        uint16_t src_port;
        int n = udp_listen(rx_port, resp, sizeof(resp), &src_ip, &src_port);
        if (n > 12 + 16) {
            if (resp[2] & 0x0F) return -1;
            int ans_count = (resp[6] << 8) | resp[7];
            if (ans_count == 0) return -1;
            int pos = 12;
            while (pos < n && resp[pos] != 0) {
                if ((resp[pos] & 0xC0) == 0xC0) pos += 2;
                else pos += resp[pos] + 1;
            }
            pos += 1;
            pos += 4;
            int rdlength = (resp[pos] << 8) | resp[pos + 1]; pos += 2;
            if (rdlength == 4 && pos + 4 <= n) {
                *ip_out = *(uint32_t *)&resp[pos];
                return 0;
            }
        }
        task_yield();
    }
    return -1;
}
