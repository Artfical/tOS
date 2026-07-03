#include "route.h"
#include "net.h"
#include "string.h"
#include "terminal.h"

static route_entry_t routes[ROUTE_MAX];
static int           route_count = 0;

static policy_rule_t policies[POLICY_MAX];
static int           policy_count = 0;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void print_ip(uint32_t ip)
{
    char buf[16]; int i = 0;
    uint8_t b[4];
    b[0] = ip & 0xFF; b[1] = (ip >> 8) & 0xFF;
    b[2] = (ip >> 16) & 0xFF; b[3] = (ip >> 24) & 0xFF;
    for (int n = 0; n < 4; n++) {
        uint8_t v = b[n];
        if (v >= 100) buf[i++] = '0' + v / 100;
        if (v >= 10)  buf[i++] = '0' + (v / 10) % 10;
        buf[i++] = '0' + v % 10;
        if (n < 3) buf[i++] = '.';
    }
    buf[i] = '\0';
    terminal_writestring(buf);
}

static int mask_to_prefix(uint32_t mask)
{
    int p = 0;
    uint32_t m = mask;
    while (m & 0x80000000U) { p++; m <<= 1; }
    return p;
}

static void print_int(int n)
{
    char buf[12]; int i = 11; buf[11] = '\0';
    if (n == 0) { terminal_putchar('0'); return; }
    int neg = (n < 0); if (neg) n = -n;
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    if (neg) buf[--i] = '-';
    terminal_writestring(buf + i);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void route_init(void)
{
    memset(routes,   0, sizeof(routes));
    memset(policies, 0, sizeof(policies));
    route_count  = 0;
    policy_count = 0;

    /* Default route: 0.0.0.0/0 via gateway, table 0 */
    if (net_gateway)
        route_add(0, 0, net_gateway, 0, 100, 0);

    /* Direct route for local subnet: 10.0.2.0/24, table 0
     * (IP4() packs octets in memory-byte order, matching wire format on
     * this little-endian target — the mask must use the same convention,
     * NOT a plain 0xFFFFFF00 host-order literal, or the prefix compare
     * in route_lookup() silently never matches.) */
    route_add(IP4(10,0,2,0), IP4(255,255,255,0),
              0, net_ip, 0, 0);
}

int route_add(uint32_t dst, uint32_t mask, uint32_t gw, uint32_t src,
              int metric, int table)
{
    if (route_count >= ROUTE_MAX) return -1;
    if (table < 0 || table >= ROUTE_TABLE_MAX) return -1;
    route_entry_t *r = &routes[route_count++];
    r->dst    = dst & mask;
    r->mask   = mask;
    r->gw     = gw;
    r->src    = src;
    r->metric = metric;
    r->table  = table;
    r->valid  = 1;
    return 0;
}

int route_del(uint32_t dst, uint32_t mask, int table)
{
    for (int i = 0; i < route_count; i++) {
        route_entry_t *r = &routes[i];
        if (r->valid && r->dst == (dst & mask) &&
            r->mask == mask && r->table == table) {
            r->valid = 0;
            return 0;
        }
    }
    return -1;
}

void route_list(void)
{
    terminal_writestring("Routing table:\n");
    terminal_writestring("  Destination     Mask             Gateway          Metric Table\n");
    for (int i = 0; i < route_count; i++) {
        route_entry_t *r = &routes[i];
        if (!r->valid) continue;
        terminal_writestring("  ");
        print_ip(r->dst);  terminal_writestring("\t");
        print_ip(r->mask); terminal_writestring("\t");
        if (r->gw) print_ip(r->gw); else terminal_writestring("0.0.0.0");
        terminal_writestring("\t");
        print_int(r->metric);
        terminal_writestring("\t");
        print_int(r->table);
        terminal_putchar('\n');
    }
}

/* Select routing table for a packet based on policy rules */
static int policy_select_table(uint32_t src, uint32_t dst)
{
    /* sort by priority (lower = higher priority); linear scan is fine for POLICY_MAX=8 */
    int best_prio = 0x7FFFFFFF, best_table = 0;
    int found = 0;
    for (int i = 0; i < policy_count; i++) {
        policy_rule_t *p = &policies[i];
        if (!p->valid) continue;
        int src_match = ((src & p->src_mask) == (p->src_ip & p->src_mask));
        int dst_match = ((dst & p->dst_mask) == (p->dst_ip & p->dst_mask));
        if (src_match && dst_match && p->priority < best_prio) {
            best_prio  = p->priority;
            best_table = p->table;
            found = 1;
        }
    }
    return found ? best_table : 0; /* default = main table */
}

uint32_t route_lookup(uint32_t src, uint32_t dst)
{
    int table = policy_select_table(src, dst);

    /* Longest prefix match within selected table, then fall back to table 0 */
    for (int pass = 0; pass < 2; pass++) {
        int t = (pass == 0) ? table : 0;
        route_entry_t *best = 0;
        int best_prefix = -1;
        for (int i = 0; i < route_count; i++) {
            route_entry_t *r = &routes[i];
            if (!r->valid || r->table != t) continue;
            if ((dst & r->mask) != r->dst) continue;
            int prefix = mask_to_prefix(r->mask);
            if (prefix > best_prefix ||
                (prefix == best_prefix && best && r->metric < best->metric)) {
                best_prefix = prefix;
                best = r;
            }
        }
        if (best) return best->gw ? best->gw : dst;
        if (t == 0) break; /* already tried main table */
    }
    return 0; /* no route */
}

/* -----------------------------------------------------------------------
 * Policy routing
 * ----------------------------------------------------------------------- */
int policy_add(uint32_t src_ip, uint32_t src_mask,
               uint32_t dst_ip, uint32_t dst_mask,
               int table, int priority)
{
    if (policy_count >= POLICY_MAX) return -1;
    if (table < 0 || table >= ROUTE_TABLE_MAX) return -1;
    policy_rule_t *p = &policies[policy_count++];
    p->src_ip   = src_ip;
    p->src_mask = src_mask;
    p->dst_ip   = dst_ip;
    p->dst_mask = dst_mask;
    p->table    = table;
    p->priority = priority;
    p->valid    = 1;
    return 0;
}

int policy_del(int priority)
{
    for (int i = 0; i < policy_count; i++) {
        if (policies[i].valid && policies[i].priority == priority) {
            policies[i].valid = 0;
            return 0;
        }
    }
    return -1;
}

void policy_list(void)
{
    terminal_writestring("Policy rules:\n");
    for (int i = 0; i < policy_count; i++) {
        policy_rule_t *p = &policies[i];
        if (!p->valid) continue;
        terminal_writestring("  prio="); print_int(p->priority);
        terminal_writestring(" src="); print_ip(p->src_ip);
        terminal_putchar('/'); print_ip(p->src_mask);
        terminal_writestring(" dst="); print_ip(p->dst_ip);
        terminal_putchar('/'); print_ip(p->dst_mask);
        terminal_writestring(" table="); print_int(p->table);
        terminal_putchar('\n');
    }
}


/* Read-only query API for Network Monitor */
int route_get_count(void) { return route_count; }
int route_get_entry(int i, route_entry_t *out)
{
    if (i < 0 || i >= route_count) return -1;
    *out = routes[i];
    return 0;
}
