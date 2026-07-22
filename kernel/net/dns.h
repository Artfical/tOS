#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* dns_resolve()'s negative return codes -- distinguishing these lets
 * callers (cmd_ping/cmd_wget) tell the user *why* resolution failed
 * instead of a single opaque "FAILED". A failed *send* propagates
 * ip_send()'s/arp_resolve()'s own code verbatim (see ip.h/arp.h)
 * instead of a generic "couldn't send", so that case reports the real
 * underlying reason (no NIC found vs. ARP got no reply) rather than
 * masking it. The DNS-specific codes below start well past every
 * network/ARP/ICMP layer's own range (see icmp.h's ICMP_ERR_TIMEOUT)
 * so a propagated lower-layer code and a DNS-specific one can never
 * collide and be misreported as each other. */
#define DNS_ERR_TIMEOUT   -20  /* query sent, but no response arrived before the deadline */
#define DNS_ERR_SERVER    -21  /* server responded with a nonzero RCODE (e.g. SERVFAIL) */
#define DNS_ERR_NO_ANSWER -22  /* server responded but with zero answers (e.g. NXDOMAIN) */
#define DNS_ERR_MALFORMED -23  /* response was truncated/corrupt */
#define DNS_ERR_NO_A      -24  /* got answers, but none were a usable A record */

int dns_resolve(const char *hostname, uint32_t *ip_out);
const char *dns_strerror(int err);

#endif
