/* Host test for kernel/net/udp.c: checksum verification, tunnel dispatch, queueing rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "ip.h"
uint8_t net_mac[6]; uint32_t net_ip;
static uint8_t cap[2048]; static int cap_len;
int ip_send(uint32_t dst, uint8_t proto, void *data, int len) { (void)dst; (void)proto; memcpy(cap, data, len); cap_len = len; return 0; }
static int wg_port = 0, wg_calls, wg_len;
int wgtun_owns_port(uint16_t p) { return wg_port && p == wg_port; }
void wgtun_rx(uint32_t s, uint16_t sp, const uint8_t *d, int l) { (void)s; (void)sp; (void)d; wg_calls++; wg_len = l; }
#include "udp.c"
static int bad;
static void check(const char *l, int ok) { if (!ok) bad++; printf("%s %s\n", ok ? "PASS" : "FAIL", l); }
int main(void)
{
    uint32_t a = IP4(10,0,2,15), b = IP4(10,0,2,2);
    net_ip = a;
    udp_init(); udp_open(5555);
    /* a datagram built by our own sender: the receiver sees src=a dst=b (loopback-style test) */
    udp_send(b, 5555, 4444, "hello-udp", 9);
    uint8_t pkt[2048]; memcpy(pkt, cap, cap_len); int n = cap_len;
    ip_hdr_t ip; memset(&ip, 0, sizeof(ip)); ip.src_ip = a; ip.dst_ip = b;
    uint8_t out[64]; uint32_t sip; uint16_t sport;

    udp_handle(&ip, pkt, n);
    check("valid checksum: delivered", udp_listen(5555, out, sizeof(out), &sip, &sport) == 9 && memcmp(out, "hello-udp", 9) == 0 && sport == 4444);
    memcpy(pkt, cap, n); pkt[n - 1] ^= 0x40;
    udp_handle(&ip, pkt, n);
    check("one payload bit flipped: dropped", udp_sockets[0].has_data == 0);
    memcpy(pkt, cap, n); ip.src_ip = IP4(6,6,6,6);
    udp_handle(&ip, pkt, n);
    check("pseudo-header mismatch (forged source address): dropped", udp_sockets[0].has_data == 0);
    ip.src_ip = a;
    memcpy(pkt, cap, n); pkt[6] = pkt[7] = 0;
    udp_handle(&ip, pkt, n);
    check("checksum 0 (not computed by the sender) is accepted", udp_listen(5555, out, sizeof(out), &sip, &sport) == 9);
    memcpy(pkt, cap, n); pkt[7] ^= 1;
    udp_handle(&ip, pkt, n);
    check("a wrong checksum value: dropped", udp_sockets[0].has_data == 0);
    /* odd length payload */
    udp_send(b, 5555, 4444, "odd", 3); memcpy(pkt, cap, cap_len); n = cap_len;
    udp_handle(&ip, pkt, n);
    check("odd-length payload with valid checksum delivered", udp_listen(5555, out, sizeof(out), &sip, &sport) == 3);
    /* tunnel port */
    wg_port = 51820; wg_calls = 0;
    udp_send(b, 51820, 51820, "tunnel-data", 11); memcpy(pkt, cap, cap_len); n = cap_len;
    udp_handle(&ip, pkt, n);
    check("a datagram for a tunnel's local port goes to wgtun_rx with its payload", wg_calls == 1 && wg_len == 11);
    memcpy(pkt, cap, n); pkt[n - 1] ^= 1; wg_calls = 0;
    udp_handle(&ip, pkt, n);
    check("a corrupted tunnel datagram does not reach the tunnel", wg_calls == 0);
    printf("failures: %d\n", bad);
    return bad != 0;
}
