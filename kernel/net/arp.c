#include "arp.h"
#include "net.h"
#include "nic.h"
#include "string.h"
#include "scheduler.h"
#include "debugmon.h"

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
    uint32_t stamp;   /* uptime ms when last confirmed */
} arp_cache_t;

#define ARP_TTL_MS 120000   /* entries older than this are re-resolved */

/* IP of the request arp_resolve() is currently waiting on (0 = none): only a
 * reply for it may install or change a mapping. */
static uint32_t arp_pending_ip;

#define ARP_CACHE_SIZE 16
static arp_cache_t arp_cache[ARP_CACHE_SIZE];

typedef struct {
    eth_hdr_t eth;
    uint16_t  htype;
    uint16_t  ptype;
    uint8_t   hlen;
    uint8_t   plen;
    uint16_t  oper;
    uint8_t   sha[6];
    uint8_t   spa[4];
    uint8_t   tha[6];
    uint8_t   tpa[4];
} __attribute__((packed)) arp_pkt_t;

void arp_init(void)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;
}

static int arp_entry_fresh(const arp_cache_t *e)
{
    return e->valid && (uint32_t)(debugmon_uptime_ms() - e->stamp) < ARP_TTL_MS;
}

/* solicited: the mapping answers a request we sent. Unsolicited packets may
 * refresh or create a mapping, but never silently rewrite a live one (that is
 * ARP cache poisoning). */
static void arp_cache_add(uint32_t ip, const uint8_t *mac, int solicited)
{
    uint32_t now = debugmon_uptime_ms();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            if (memcmp(arp_cache[i].mac, mac, 6) == 0) { arp_cache[i].stamp = now; return; }
            if (solicited || !arp_entry_fresh(&arp_cache[i])) {
                memcpy(arp_cache[i].mac, mac, 6);
                arp_cache[i].stamp = now;
            }
            return;
        }
    }
    /* New mapping: take a free or expired slot, otherwise evict the oldest
     * (a full cache used to refuse every new host for good). */
    int victim = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_entry_fresh(&arp_cache[i])) { victim = i; break; }
        if ((uint32_t)(now - arp_cache[i].stamp) > (uint32_t)(now - arp_cache[victim].stamp)) victim = i;
    }
    arp_cache[victim].valid = 1;
    arp_cache[victim].ip = ip;
    memcpy(arp_cache[victim].mac, mac, 6);
    arp_cache[victim].stamp = now;
}

static void arp_send_request(uint32_t ip)
{
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t *arp = (arp_pkt_t *)buf;
    memset(buf, 0, sizeof(arp_pkt_t));
    memset(arp->eth.dst, 0xFF, 6);
    memcpy(arp->eth.src, net_mac, 6);
    arp->eth.type = htons(ETHERTYPE_ARP);
    arp->htype = htons(1);
    arp->ptype = htons(ETHERTYPE_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1);
    memcpy(arp->sha, net_mac, 6);
    *(uint32_t *)arp->spa = net_ip;
    *(uint32_t *)arp->tpa = ip;
    nic_transmit(buf, sizeof(arp_pkt_t));
}

int arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    if (!nic_send || !nic_poll) return ARP_ERR_NO_NIC;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_entry_fresh(&arp_cache[i]) && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 0;
        }
    }
    arp_pending_ip = ip;
    arp_send_request(ip);
    /* Wall-clock timeout, not an iteration count — how long a single
     * nic_poll() call takes varies wildly by driver/hypervisor (a VM's
     * emulated NIC can be far slower per call than QEMU's), so a fixed
     * retry count is either too short (fails before a real NAT'd reply
     * arrives) or, if set high enough to be safe, can turn into a
     * multi-minute hang on a slow driver. 3 real seconds either way. */
    uint32_t deadline = debugmon_uptime_ms() + 3000;
    while (debugmon_uptime_ms() < deadline) {
        uint8_t pkt[1536];
        int len = nic_poll(pkt, sizeof(pkt));
        if (len > 0) {
            eth_hdr_t *eth = (eth_hdr_t *)pkt;
            if (ntohs(eth->type) == ETHERTYPE_ARP)
                arp_handle(pkt, len);
        }
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_entry_fresh(&arp_cache[i]) && arp_cache[i].ip == ip) {
                arp_pending_ip = 0;
                memcpy(mac_out, arp_cache[i].mac, 6);
                return 0;
            }
        }
        /* No task_yield() here -- reachable from a ring3 .t program's
         * blocking syscall (tos_net_connect() -> tcp_connect() ->
         * arp_resolve()), and a nested software interrupt from inside
         * that syscall's own int $0x80 trap-gate handler is a confirmed
         * reentrancy bug (see kernel/drivers/input/keyboard.c). nic_poll()
         * above already polls the NIC directly. */
    }
    arp_pending_ip = 0;
    return ARP_ERR_TIMEOUT;
}

const char *arp_resolve_strerror(int err)
{
    switch (err) {
        case ARP_ERR_NO_NIC:  return "no network card found";
        case ARP_ERR_TIMEOUT: return "ARP request timed out, no reply";
        default:               return "unknown error";
    }
}

void arp_handle(uint8_t *data, int len)
{
    /* Whole packet present and describing Ethernet/IPv4 (the old code read
     * the fields without looking at len or the address sizes at all). */
    if (len < (int)sizeof(arp_pkt_t)) return;
    arp_pkt_t *arp = (arp_pkt_t *)data;
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != ETHERTYPE_IP) return;
    if (arp->hlen != 6 || arp->plen != 4) return;
    uint16_t oper = ntohs(arp->oper);
    if (oper != 1 && oper != 2) return;

    uint32_t src_ip = *(uint32_t *)arp->spa;
    /* Never learn broadcast/zero/our own address or a multicast/zero MAC. */
    if (src_ip == 0 || src_ip == 0xFFFFFFFFu || src_ip == net_ip) return;
    if ((arp->sha[0] & 1) || (!arp->sha[0] && !arp->sha[1] && !arp->sha[2] &&
                              !arp->sha[3] && !arp->sha[4] && !arp->sha[5])) return;

    if (oper == 2) {
        /* A reply only counts for the address we asked about, and only
         * when it is addressed to us. */
        if (src_ip != arp_pending_ip) {
            for (int i = 0; i < ARP_CACHE_SIZE; i++)
                if (arp_cache[i].valid && arp_cache[i].ip == src_ip &&
                    memcmp(arp_cache[i].mac, arp->sha, 6) == 0) arp_cache[i].stamp = debugmon_uptime_ms();
            return;
        }
        arp_cache_add(src_ip, arp->sha, 1);
        return;
    }

    arp_cache_add(src_ip, arp->sha, 0);
    if (*(uint32_t *)arp->tpa == net_ip) {
        uint8_t buf[sizeof(arp_pkt_t)];
        arp_pkt_t *reply = (arp_pkt_t *)buf;
        memset(buf, 0, sizeof(arp_pkt_t));
        memcpy(reply->eth.dst, arp->sha, 6);
        memcpy(reply->eth.src, net_mac, 6);
        reply->eth.type = htons(ETHERTYPE_ARP);
        reply->htype = htons(1);
        reply->ptype = htons(ETHERTYPE_IP);
        reply->hlen = 6;
        reply->plen = 4;
        reply->oper = htons(2);
        memcpy(reply->sha, net_mac, 6);
        *(uint32_t *)reply->spa = net_ip;
        memcpy(reply->tha, arp->sha, 6);
        *(uint32_t *)reply->tpa = src_ip;
        nic_transmit(buf, sizeof(arp_pkt_t));
    }
}
