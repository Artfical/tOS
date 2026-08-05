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

    int src = udp_send(net_dns, DNS_PORT, rx_port, pkt, off);
    if (src != 0) return src; /* propagate ip_send()/arp_resolve()'s own specific code */

    /* Wall-clock timeout, not an iteration count — see arp_resolve()
     * for why a fixed retry count is unreliable across drivers. */
    uint32_t deadline = debugmon_uptime_ms() + 5000;
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
            /* RCODE is the low nibble of byte 3 (RA|Z|RCODE), not byte 2
             * (QR|Opcode|AA|TC|RD) -- our query always sets RD=1, and a
             * real server echoes that bit straight back, so checking
             * byte 2 here made a perfectly successful NOERROR response
             * always look like a server error. */
            if (resp[3] & 0x0F) return DNS_ERR_SERVER;
            int ans_count = (resp[6] << 8) | resp[7];
            if (ans_count == 0) return DNS_ERR_NO_ANSWER;
            /* resp is a fixed-size stack buffer filled straight from
             * a UDP datagram sent by whatever (spoofable, attacker-
             * controlled) server answered on rx_port -- every label
             * length byte and the rdlength field below come directly
             * from that untrusted response, so pos must be checked
             * against n before each read, not just at the final IP
             * copy. A crafted/truncated response used to let pos walk
             * past n (a label's own length byte can push it up to 63
             * bytes over in one step) and read resp[pos]/resp[pos+1]
             * out of bounds -- a remote out-of-bounds stack read. */
            int pos = 12;
            int qname_compressed = 0;
            while (pos < n && resp[pos] != 0) {
                if ((resp[pos] & 0xC0) == 0xC0) { pos += 2; qname_compressed = 1; break; }
                if (pos + resp[pos] + 1 > n) return DNS_ERR_MALFORMED;
                pos += resp[pos] + 1;
            }
            if (pos >= n) return DNS_ERR_MALFORMED;
            if (!qname_compressed) pos += 1; /* skip the name's null terminator */
            if (pos + 4 > n) return DNS_ERR_MALFORMED;
            pos += 4; /* qtype + qclass */

            /* Walk each answer RR looking for the first real A record.
             * The previous version jumped straight from here to
             * reading two bytes as RDLENGTH, but a real answer RR
             * starts with its own NAME (almost always a 2-byte
             * compression pointer back to the question, not the
             * literal question bytes), then TYPE(2)/CLASS(2)/TTL(4),
             * and only then RDLENGTH -- skipping all of that made the
             * compression pointer's own bytes get misread as
             * RDLENGTH, so a real-world DNS response (which always
             * uses name compression) could never match `rdlength==4`
             * and resolution failed unconditionally. Also, if the
             * first answer is a CNAME (common for aliased hosts)
             * rather than an A record, it must be skipped in favor of
             * a later answer rather than giving up immediately. */
            for (int a = 0; a < ans_count && pos < n; a++) {
                if ((resp[pos] & 0xC0) == 0xC0) {
                    if (pos + 2 > n) return DNS_ERR_MALFORMED;
                    pos += 2;
                } else {
                    while (pos < n && resp[pos] != 0) {
                        if (pos + resp[pos] + 1 > n) return DNS_ERR_MALFORMED;
                        pos += resp[pos] + 1;
                    }
                    if (pos >= n) return DNS_ERR_MALFORMED;
                    pos += 1;
                }
                if (pos + 10 > n) return DNS_ERR_MALFORMED; /* type+class+ttl+rdlength */
                uint16_t rtype = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
                pos += 2 + 2 + 4; /* type, class, ttl */
                int rdlength = (resp[pos] << 8) | resp[pos + 1];
                pos += 2;
                if (pos + rdlength > n) return DNS_ERR_MALFORMED;
                if (rtype == 1 && rdlength == 4) {
                    *ip_out = *(uint32_t *)&resp[pos];
                    return 0;
                }
                pos += rdlength; /* not an A record -- try the next answer */
            }
            return DNS_ERR_NO_A;
        }
        /* No task_yield() here -- reachable from a ring3 .t program's
         * blocking tos_net_resolve() (SYS_NET_RESOLVE, via int $0x80);
         * see kernel/drivers/input/keyboard.c for why a nested software
         * interrupt from inside a syscall's own trap-gate handler is
         * unsafe. The nic_poll() call above this loop already polls
         * the NIC directly. */
    }
    return DNS_ERR_TIMEOUT;
}

const char *dns_strerror(int err)
{
    switch (err) {
        case DNS_ERR_TIMEOUT:   return "no response from DNS server (timed out)";
        case DNS_ERR_SERVER:    return "DNS server returned an error";
        case DNS_ERR_NO_ANSWER: return "no such host (no records returned)";
        case DNS_ERR_MALFORMED: return "malformed response from DNS server";
        case DNS_ERR_NO_A:      return "host has no IPv4 (A) address";
        case IP_ERR_NOMEM:      return "out of memory building packet";
        default:                return arp_resolve_strerror(err); /* ARP_ERR_* (couldn't send at all) */
    }
}
