#ifndef DHCP_H
#define DHCP_H

/* dhcp_configure()'s negative return codes -- numbered well past every
 * other layer's range (see icmp.h/dns.h) so a propagated lower-layer
 * code can never collide with one of these. DHCP has no lower layer
 * of its own to propagate (it talks straight to nic_send/nic_poll,
 * like arp.c does), so these three cover its entire failure surface. */
#define DHCP_ERR_NO_NIC  -30 /* no NIC attached, can't even broadcast DISCOVER */
#define DHCP_ERR_TIMEOUT -31 /* no OFFER/ACK arrived before the deadline */
#define DHCP_ERR_NAK     -32 /* server explicitly refused the requested lease */

/* Broadcasts DHCPDISCOVER, negotiates a lease (DISCOVER/OFFER/REQUEST/ACK),
 * and on success overwrites net_ip/net_gateway/net_dns/net_netmask with
 * the leased values. Leaves those globals at their compiled-in defaults
 * on failure, so callers that don't check the return value still get
 * the old QEMU-slirp-shaped behavior rather than an all-zero config. */
int dhcp_configure(void);
const char *dhcp_strerror(int err);

#endif
