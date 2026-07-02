#include "fw.h"
#include "net.h"
#include "icmp.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

static fw_rule_t  fw_rules[FW_RULE_MAX];
static int        fw_rule_count = 0;

static nat_rule_t nat_rules[NAT_MAX];
static int        nat_rule_count = 0;

static ct_entry_t ct_table[CT_MAX];

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

static void print_uint16(uint16_t n)
{
    char buf[8]; int i = 7; buf[7] = '\0';
    if (n == 0) { terminal_putchar('0'); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

static void print_int(int n)
{
    char buf[12]; int i = 11; buf[11] = '\0';
    if (n == 0) { terminal_putchar('0'); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

/* Extract src/dst port from TCP/UDP payload (first 4 bytes) */
static void get_ports(void *payload, int plen, uint8_t proto,
                      uint16_t *sp, uint16_t *dp)
{
    *sp = 0; *dp = 0;
    if ((proto == 6 || proto == 17 || proto == 33 || proto == 136) && plen >= 4) {
        uint8_t *p = (uint8_t *)payload;
        *sp = (uint16_t)((p[0] << 8) | p[1]);
        *dp = (uint16_t)((p[2] << 8) | p[3]);
    }
}

static uint16_t ip_checksum(const uint8_t *buf, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len - 1; i += 2)
        sum += ((uint16_t)buf[i] << 8) | buf[i + 1];
    if (len & 1) sum += (uint16_t)buf[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* -----------------------------------------------------------------------
 * Conntrack
 * ----------------------------------------------------------------------- */
static ct_entry_t *ct_lookup(uint8_t proto,
                              uint32_t sip, uint16_t sp,
                              uint32_t dip, uint16_t dp)
{
    for (int i = 0; i < CT_MAX; i++) {
        ct_entry_t *c = &ct_table[i];
        if (!c->valid || c->proto != proto) continue;
        if (c->src_ip == sip && c->src_port == sp &&
            c->dst_ip == dip && c->dst_port == dp) return c;
    }
    return 0;
}

static ct_entry_t *ct_alloc(void)
{
    for (int i = 0; i < CT_MAX; i++)
        if (!ct_table[i].valid) return &ct_table[i];
    /* evict slot 0 */
    memset(&ct_table[0], 0, sizeof(ct_entry_t));
    return &ct_table[0];
}

static ct_entry_t *ct_track(uint8_t proto,
                             uint32_t sip, uint16_t sp,
                             uint32_t dip, uint16_t dp)
{
    ct_entry_t *c = ct_lookup(proto, sip, sp, dip, dp);
    if (c) return c;
    c = ct_alloc();
    c->proto     = proto;
    c->src_ip    = sip; c->src_port = sp;
    c->dst_ip    = dip; c->dst_port = dp;
    c->valid     = 1;
    return c;
}

void ct_dump(void)
{
    terminal_writestring("Conntrack table:\n");
    int any = 0;
    for (int i = 0; i < CT_MAX; i++) {
        ct_entry_t *c = &ct_table[i];
        if (!c->valid) continue;
        any = 1;
        terminal_writestring("  proto="); print_int(c->proto);
        terminal_writestring(" "); print_ip(c->src_ip);
        terminal_putchar(':'); print_uint16(c->src_port);
        terminal_writestring(" -> "); print_ip(c->dst_ip);
        terminal_putchar(':'); print_uint16(c->dst_port);
        if (c->nat_src_ip || c->nat_dst_ip) {
            terminal_writestring(" NAT ");
            if (c->nat_src_ip) { print_ip(c->nat_src_ip); terminal_putchar(':'); print_uint16(c->nat_src_port); }
            if (c->nat_dst_ip) { terminal_writestring("->"); print_ip(c->nat_dst_ip); terminal_putchar(':'); print_uint16(c->nat_dst_port); }
        }
        terminal_putchar('\n');
    }
    if (!any) terminal_writestring("  (empty)\n");
}

/* -----------------------------------------------------------------------
 * Firewall init
 * ----------------------------------------------------------------------- */
void fw_init(void)
{
    memset(fw_rules,  0, sizeof(fw_rules));
    memset(nat_rules, 0, sizeof(nat_rules));
    memset(ct_table,  0, sizeof(ct_table));
    fw_rule_count  = 0;
    nat_rule_count = 0;
    /* Default: ACCEPT everything */
}

/* -----------------------------------------------------------------------
 * Firewall rules
 * ----------------------------------------------------------------------- */
int fw_rule_add(uint8_t proto,
                uint32_t src_ip, uint32_t src_mask,
                uint32_t dst_ip, uint32_t dst_mask,
                uint16_t src_port, uint16_t dst_port,
                uint8_t action)
{
    if (fw_rule_count >= FW_RULE_MAX) return -1;
    fw_rule_t *r = &fw_rules[fw_rule_count++];
    r->proto    = proto;
    r->src_ip   = src_ip;   r->src_mask = src_mask;
    r->dst_ip   = dst_ip;   r->dst_mask = dst_mask;
    r->src_port = src_port; r->dst_port = dst_port;
    r->action   = action;
    r->valid    = 1;
    return fw_rule_count - 1;
}

int fw_rule_del(int index)
{
    if (index < 0 || index >= fw_rule_count) return -1;
    fw_rules[index].valid = 0;
    return 0;
}

void fw_rule_list(void)
{
    terminal_writestring("Firewall rules:\n");
    for (int i = 0; i < fw_rule_count; i++) {
        fw_rule_t *r = &fw_rules[i];
        if (!r->valid) continue;
        terminal_writestring("  ["); print_int(i); terminal_writestring("] ");
        terminal_writestring("proto="); print_int(r->proto);
        terminal_writestring(" src="); print_ip(r->src_ip);
        terminal_putchar('/'); print_ip(r->src_mask);
        if (r->src_port) { terminal_putchar(':'); print_uint16(r->src_port); }
        terminal_writestring(" dst="); print_ip(r->dst_ip);
        terminal_putchar('/'); print_ip(r->dst_mask);
        if (r->dst_port) { terminal_putchar(':'); print_uint16(r->dst_port); }
        terminal_writestring(" -> ");
        terminal_writestring(r->action == FW_ACCEPT ? "ACCEPT" :
                             r->action == FW_DROP   ? "DROP"   : "REJECT");
        terminal_putchar('\n');
    }
}

/* -----------------------------------------------------------------------
 * NAT rules
 * ----------------------------------------------------------------------- */
int nat_rule_add(nat_type_t type,
                 uint32_t match_ip, uint32_t match_mask, uint16_t match_port,
                 uint32_t new_ip, uint16_t new_port)
{
    if (nat_rule_count >= NAT_MAX) return -1;
    nat_rule_t *n = &nat_rules[nat_rule_count++];
    n->type       = type;
    n->match_ip   = match_ip;   n->match_mask  = match_mask;
    n->match_port = match_port;
    n->new_ip     = new_ip;     n->new_port    = new_port;
    n->valid      = 1;
    return nat_rule_count - 1;
}

int nat_rule_del(int index)
{
    if (index < 0 || index >= nat_rule_count) return -1;
    nat_rules[index].valid = 0;
    return 0;
}

void nat_rule_list(void)
{
    terminal_writestring("NAT rules:\n");
    for (int i = 0; i < nat_rule_count; i++) {
        nat_rule_t *n = &nat_rules[i];
        if (!n->valid) continue;
        terminal_writestring("  ["); print_int(i); terminal_writestring("] ");
        terminal_writestring(n->type == NAT_SNAT ? "SNAT" : "DNAT");
        terminal_writestring(" match="); print_ip(n->match_ip);
        terminal_putchar('/'); print_ip(n->match_mask);
        if (n->match_port) { terminal_putchar(':'); print_uint16(n->match_port); }
        terminal_writestring(" -> "); print_ip(n->new_ip);
        if (n->new_port) { terminal_putchar(':'); print_uint16(n->new_port); }
        terminal_putchar('\n');
    }
}

/* -----------------------------------------------------------------------
 * fw_rx — called from ip_handle() before protocol dispatch
 * ----------------------------------------------------------------------- */
int fw_rx(ip_hdr_t *ip, void *payload, int payload_len)
{
    uint32_t sip = ip->src_ip;
    uint32_t dip = ip->dst_ip;
    uint8_t  proto = ip->protocol;
    uint16_t sp = 0, dp = 0;
    get_ports(payload, payload_len, proto, &sp, &dp);

    /* Track connection */
    ct_entry_t *ct = ct_track(proto, sip, sp, dip, dp);

    /* Apply DNAT rules */
    for (int i = 0; i < nat_rule_count; i++) {
        nat_rule_t *n = &nat_rules[i];
        if (!n->valid || n->type != NAT_DNAT) continue;
        if ((dip & n->match_mask) != (n->match_ip & n->match_mask)) continue;
        if (n->match_port && dp != n->match_port) continue;
        /* Translate destination */
        ct->nat_dst_ip   = n->new_ip;
        ct->nat_dst_port = n->new_port ? n->new_port : dp;
        ip->dst_ip = n->new_ip;
        /* Recompute IP checksum */
        ip->checksum = 0;
        int ihl = (ip->ver_ihl & 0xF) * 4;
        ip->checksum = htons(ip_checksum((uint8_t *)ip, ihl));
        break;
    }

    /* Apply firewall rules (first match wins) */
    for (int i = 0; i < fw_rule_count; i++) {
        fw_rule_t *r = &fw_rules[i];
        if (!r->valid) continue;
        if (r->proto && r->proto != proto) continue;
        if ((sip & r->src_mask) != (r->src_ip & r->src_mask)) continue;
        if ((dip & r->dst_mask) != (r->dst_ip & r->dst_mask)) continue;
        if (r->src_port && sp != r->src_port) continue;
        if (r->dst_port && dp != r->dst_port) continue;
        if (r->action == FW_DROP)   return FW_DROP;
        if (r->action == FW_REJECT) return FW_REJECT;
        return FW_ACCEPT;
    }
    return FW_ACCEPT; /* default policy */
}

/* -----------------------------------------------------------------------
 * fw_tx — SNAT on egress
 * ----------------------------------------------------------------------- */
void fw_tx(ip_hdr_t *ip, void *payload, int payload_len)
{
    uint32_t sip = ip->src_ip;
    uint16_t sp = 0, dp = 0;
    get_ports(payload, payload_len, ip->protocol, &sp, &dp);

    for (int i = 0; i < nat_rule_count; i++) {
        nat_rule_t *n = &nat_rules[i];
        if (!n->valid || n->type != NAT_SNAT) continue;
        if ((sip & n->match_mask) != (n->match_ip & n->match_mask)) continue;
        if (n->match_port && sp != n->match_port) continue;
        /* Record translation in conntrack */
        ct_entry_t *ct = ct_track(ip->protocol, sip, sp, ip->dst_ip, dp);
        ct->nat_src_ip   = n->new_ip;
        ct->nat_src_port = n->new_port ? n->new_port : sp;
        /* Translate source */
        ip->src_ip = n->new_ip;
        ip->checksum = 0;
        int ihl = (ip->ver_ihl & 0xF) * 4;
        ip->checksum = htons(ip_checksum((uint8_t *)ip, ihl));
        break;
    }
}

/* Read-only query API for Network Monitor */
int fw_get_rule_count(void) { return fw_rule_count; }
int fw_get_rule(int i, fw_rule_t *out)
{
    if (i < 0 || i >= fw_rule_count) return -1;
    *out = fw_rules[i];
    return 0;
}
