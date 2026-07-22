#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* dns_resolve()'s negative return codes -- distinguishing these lets
 * callers (cmd_ping/cmd_wget) tell the user *why* resolution failed
 * instead of a single opaque "FAILED", since each of these points at
 * a different place to look (no route/ARP vs. nothing ever answered
 * vs. the DNS server itself rejected the query vs. a malformed reply). */
#define DNS_ERR_SEND      -1  /* couldn't even send the query (routing/ARP failure) */
#define DNS_ERR_TIMEOUT   -2  /* no response arrived before the deadline */
#define DNS_ERR_SERVER    -3  /* server responded with a nonzero RCODE (e.g. SERVFAIL) */
#define DNS_ERR_NO_ANSWER -4  /* server responded but with zero answers (e.g. NXDOMAIN) */
#define DNS_ERR_MALFORMED -5  /* response was truncated/corrupt */
#define DNS_ERR_NO_A      -6  /* got answers, but none were a usable A record */

int dns_resolve(const char *hostname, uint32_t *ip_out);
const char *dns_strerror(int err);

#endif
