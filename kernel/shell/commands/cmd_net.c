#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "dns.h"
#include "icmp.h"
#include "http.h"
#include "tsharp.h"
#include "micropython.h"
#include "memory.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

static uint32_t parse_ip(const char *s)
{
    uint32_t ip = 0;
    int shift = 0, val = 0;
    while (*s) {
        if (*s == '.') { ip |= (val & 0xFF) << shift; shift += 8; val = 0; }
        else if (*s >= '0' && *s <= '9') val = val * 10 + (*s - '0');
        else return 0;
        s++;
    }
    ip |= (val & 0xFF) << shift;
    return ip;
}

void cmd_ping(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: ping <ip> or <hostname>\n"); return; }
    uint32_t ip;
    int is_ip = 1;
    for (char *p = args[1]; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { is_ip = 0; break; }
    if (is_ip) ip = parse_ip(args[1]);
    else {
        terminal_writestring("Resolving... ");
        if (dns_resolve(args[1], &ip) != 0)
            { terminal_writestring("FAILED\n"); return; }
        terminal_writestring("OK\n");
    }
    terminal_writestring("Pinging... ");
    if (icmp_ping(ip) == 0) terminal_writestring("Reply received\n");
    else terminal_writestring("No reply\n");
}

void cmd_wget(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: wget <url>\n");
        terminal_writestring("  e.g. wget http://example.com/file\n");
        return;
    }
    const char *url = args[1];
    if (strncmp(url, "http://", 7) != 0)
        { terminal_writestring("wget: Only http:// supported\n"); return; }
    url += 7;

    char host[256], path[256];
    int i = 0, j = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) host[i++] = *url++;
    host[i] = '\0';

    uint16_t port = 80;
    if (*url == ':') { url++; port = 0; while (*url >= '0' && *url <= '9') { port = port * 10 + (*url - '0'); url++; } }

    if (*url == '/') { while (*url && j < 255) path[j++] = *url++; path[j] = '\0'; }
    else { path[0] = '/'; path[1] = '\0'; }

    const char *fname = path;
    for (const char *p = path; *p; p++) if (*p == '/') fname = p + 1;
    if (*fname == '\0') fname = "downloaded";

    terminal_writestring("Resolving... ");
    uint32_t ip;
    if (dns_resolve(host, &ip) != 0) { terminal_writestring("FAILED\n"); return; }
    terminal_writestring("OK\nConnecting... ");

    uint8_t resp[4096];
    int n = http_get(host, port, path, resp, sizeof(resp) - 1);
    if (n <= 0) { terminal_writestring("FAILED\n"); return; }
    resp[n] = '\0';
    terminal_writestring("OK (");
    print_num(n);
    terminal_writestring(" bytes)\n");

    if (ramfs_create(fname) == 0) {
        ramfs_write(fname, (char *)resp, n, 0);
        terminal_writestring("Saved to: ");
        terminal_writestring(fname);
        terminal_writestring("\n");
    } else {
        terminal_writestring("(Cannot write, showing content)\n");
        terminal_writestring((char *)resp);
        terminal_putchar('\n');
    }
}

void cmd_tsharp(int argc, char **args)
{
    if (argc > 1) tsharp_run_file(args[1]);
    else tsharp_run_interactive();
}

void cmd_python(int argc, char **args)
{
    if (argc > 1) micropython_run_file(args[1]);
    else micropython_run_repl();
}

/* -----------------------------------------------------------------------
 * New protocol test commands (v0.9.39)
 * ----------------------------------------------------------------------- */
#include "sctp.h"
#include "dccp.h"
#include "udplite.h"
#include "icmpv6.h"
#include "ipsec.h"
#include "ip6.h"
#include "vlan.h"
#include "bridge.h"
#include "bonding.h"
#include "ipx.h"

void cmd_sctp_connect(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: sctp_connect <ip> <port>\n");
        return;
    }
    uint32_t ip = parse_ip(args[1]);
    int port = 0;
    for (char *p = args[2]; *p; p++) port = port * 10 + (*p - '0');
    terminal_writestring("SCTP: connecting to ");
    terminal_writestring(args[1]);
    terminal_writestring(":");
    terminal_writestring(args[2]);
    terminal_writestring(" ...\n");
    if (sctp_connect(ip, (uint16_t)port) == 0)
        terminal_writestring("SCTP: association established\n");
    else
        terminal_writestring("SCTP: connection failed\n");
}

void cmd_sctp_send(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: sctp_send <data>\n"); return; }
    if (sctp_send(args[1], strlen(args[1])) == 0)
        terminal_writestring("SCTP: data sent\n");
    else
        terminal_writestring("SCTP: send failed (not connected?)\n");
}

void cmd_sctp_close(int argc, char **args)
{
    (void)argc; (void)args;
    sctp_close();
    terminal_writestring("SCTP: association closed\n");
}

void cmd_dccp_connect(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: dccp_connect <ip> <port>\n");
        return;
    }
    uint32_t ip = parse_ip(args[1]);
    int port = 0;
    for (char *p = args[2]; *p; p++) port = port * 10 + (*p - '0');
    terminal_writestring("DCCP: connecting to ");
    terminal_writestring(args[1]);
    terminal_writestring(":");
    terminal_writestring(args[2]);
    terminal_writestring(" ...\n");
    if (dccp_connect(ip, (uint16_t)port) == 0)
        terminal_writestring("DCCP: connection open\n");
    else
        terminal_writestring("DCCP: connection failed\n");
}

void cmd_dccp_send(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: dccp_send <data>\n"); return; }
    if (dccp_send(args[1], strlen(args[1])) == 0)
        terminal_writestring("DCCP: datagram sent\n");
    else
        terminal_writestring("DCCP: send failed\n");
}

void cmd_udplite_send(int argc, char **args)
{
    if (argc < 4) {
        terminal_writestring("usage: udplite_send <ip> <port> <data> [coverage]\n");
        return;
    }
    uint32_t ip = parse_ip(args[1]);
    int port = 0;
    for (char *p = args[2]; *p; p++) port = port * 10 + (*p - '0');
    uint16_t coverage = 0;
    if (argc >= 5)
        for (char *p = args[4]; *p; p++) coverage = coverage * 10 + (*p - '0');
    if (udplite_send(ip, (uint16_t)port, 49136, args[3], strlen(args[3]), coverage) == 0)
        terminal_writestring("UDP-Lite: datagram sent\n");
    else
        terminal_writestring("UDP-Lite: send failed\n");
}

void cmd_ping6(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ping6 <ipv6-address>\n");
        terminal_writestring("  e.g. ping6 fe80::1\n");
        return;
    }
    uint8_t addr[16];
    if (ip6_parse(args[1], addr) != 0) {
        terminal_writestring("ping6: invalid IPv6 address\n");
        return;
    }
    terminal_writestring("ping6 ");
    terminal_writestring(args[1]);
    terminal_writestring(" ...\n");
    if (icmpv6_ping6(addr) == 0)
        terminal_writestring("Reply received\n");
    else
        terminal_writestring("No reply\n");
}

void cmd_ip6addr(int argc, char **args)
{
    (void)argc; (void)args;
    extern uint8_t net_ip6[16];
    char buf[42];
    ip6_fmt(net_ip6, buf);
    terminal_writestring("IPv6 link-local: ");
    terminal_writestring(buf);
    terminal_writestring("\n");
}

void cmd_ipsec_sa(int argc, char **args)
{
    (void)argc; (void)args;
    ipsec_dump_sa();
}

/* -----------------------------------------------------------------------
 * VLAN commands
 * vlan add <vid>  /  vlan rm <vid>  /  vlan list
 * ----------------------------------------------------------------------- */
void cmd_vlan(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: vlan add <vid> | vlan rm <vid> | vlan list\n");
        return;
    }
    if (strcmp(args[1], "add") == 0) {
        if (argc < 3) { terminal_writestring("usage: vlan add <vid>\n"); return; }
        int vid = 0;
        for (char *p = args[2]; *p; p++) vid = vid * 10 + (*p - '0');
        if (vlan_add((uint16_t)vid) == 0) {
            terminal_writestring("VLAN ");
            terminal_writestring(args[2]);
            terminal_writestring(" added\n");
        } else {
            terminal_writestring("vlan add: failed (full or invalid vid)\n");
        }
    } else if (strcmp(args[1], "rm") == 0 || strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: vlan rm <vid>\n"); return; }
        int vid = 0;
        for (char *p = args[2]; *p; p++) vid = vid * 10 + (*p - '0');
        if (vlan_remove((uint16_t)vid) == 0) {
            terminal_writestring("VLAN ");
            terminal_writestring(args[2]);
            terminal_writestring(" removed\n");
        } else {
            terminal_writestring("vlan rm: not found\n");
        }
    } else if (strcmp(args[1], "list") == 0) {
        vlan_list();
    } else {
        terminal_writestring("vlan: unknown subcommand. Use add/rm/list\n");
    }
}

/* -----------------------------------------------------------------------
 * Bridge commands
 * bridge create <name>  /  bridge del <name>
 * bridge addif <br> <if>  /  bridge delif <br> <if>
 * bridge list
 * ----------------------------------------------------------------------- */
void cmd_bridge(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: bridge create <name> | bridge del <name>\n");
        terminal_writestring("       bridge addif <br> <if> | bridge delif <br> <if>\n");
        terminal_writestring("       bridge list\n");
        return;
    }
    if (strcmp(args[1], "create") == 0) {
        if (argc < 3) { terminal_writestring("usage: bridge create <name>\n"); return; }
        if (bridge_create(args[2]) == 0) {
            terminal_writestring("bridge ");
            terminal_writestring(args[2]);
            terminal_writestring(" created\n");
        } else {
            terminal_writestring("bridge create: failed\n");
        }
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: bridge del <name>\n"); return; }
        if (bridge_destroy(args[2]) == 0) {
            terminal_writestring("bridge ");
            terminal_writestring(args[2]);
            terminal_writestring(" deleted\n");
        } else {
            terminal_writestring("bridge del: not found\n");
        }
    } else if (strcmp(args[1], "addif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bridge addif <br> <if>\n"); return; }
        if (bridge_add_if(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" added to bridge ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bridge addif: failed\n");
        }
    } else if (strcmp(args[1], "delif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bridge delif <br> <if>\n"); return; }
        if (bridge_remove_if(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" removed from bridge ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bridge delif: failed\n");
        }
    } else if (strcmp(args[1], "list") == 0) {
        bridge_list();
    } else {
        terminal_writestring("bridge: unknown subcommand\n");
    }
}

/* -----------------------------------------------------------------------
 * Bonding commands
 * bond create <name> [failover|balance]
 * bond del <name>
 * bond addif <bond> <if>
 * bond delif <bond> <if>
 * bond failover <name>
 * bond list
 * ----------------------------------------------------------------------- */
void cmd_bond(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: bond create <name> [failover|balance]\n");
        terminal_writestring("       bond del <name>\n");
        terminal_writestring("       bond addif <bond> <if> | bond delif <bond> <if>\n");
        terminal_writestring("       bond failover <name> | bond list\n");
        return;
    }
    if (strcmp(args[1], "create") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond create <name> [failover|balance]\n"); return; }
        bond_mode_t mode = BOND_MODE_FAILOVER;
        if (argc >= 4 && strcmp(args[3], "balance") == 0)
            mode = BOND_MODE_BALANCE;
        if (bond_create(args[2], mode) == 0) {
            terminal_writestring("bond ");
            terminal_writestring(args[2]);
            terminal_writestring(" created (mode=");
            terminal_writestring(mode == BOND_MODE_FAILOVER ? "failover" : "balance");
            terminal_writestring(")\n");
        } else {
            terminal_writestring("bond create: failed\n");
        }
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond del <name>\n"); return; }
        if (bond_destroy(args[2]) == 0) {
            terminal_writestring("bond ");
            terminal_writestring(args[2]);
            terminal_writestring(" deleted\n");
        } else {
            terminal_writestring("bond del: not found\n");
        }
    } else if (strcmp(args[1], "addif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bond addif <bond> <if>\n"); return; }
        if (bond_add_slave(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" added to bond ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bond addif: failed\n");
        }
    } else if (strcmp(args[1], "delif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bond delif <bond> <if>\n"); return; }
        if (bond_remove_slave(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" removed from bond ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bond delif: failed\n");
        }
    } else if (strcmp(args[1], "failover") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond failover <name>\n"); return; }
        if (bond_failover(args[2]) != 0)
            terminal_writestring("bond failover: failed (need >= 2 slaves)\n");
    } else if (strcmp(args[1], "list") == 0) {
        bond_list();
    } else {
        terminal_writestring("bond: unknown subcommand\n");
    }
}

/* -----------------------------------------------------------------------
 * IPX command
 * ipx send <dst_node_hex> <data>   e.g. ipx send FFFFFFFFFFFF hello
 * ----------------------------------------------------------------------- */
static int parse_hex_byte(const char *s)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else return -1;
    }
    return v;
}

void cmd_ipx(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ipx send <dst_node_hex12> <data>\n");
        terminal_writestring("  e.g. ipx send FFFFFFFFFFFF hello\n");
        return;
    }
    if (strcmp(args[1], "send") == 0) {
        if (argc < 4) { terminal_writestring("usage: ipx send <node12hex> <data>\n"); return; }
        const char *hex = args[2];
        if (strlen(hex) != 12) { terminal_writestring("ipx: node must be 12 hex chars\n"); return; }
        uint8_t node[6];
        for (int i = 0; i < 6; i++) {
            int b = parse_hex_byte(hex + i * 2);
            if (b < 0) { terminal_writestring("ipx: invalid hex in node\n"); return; }
            node[i] = (uint8_t)b;
        }
        static const uint8_t zero_net[4] = {0,0,0,0};
        const char *data = args[3];
        if (ipx_send(node, zero_net, IPX_SOCK_ECHO, IPX_SOCK_ECHO,
                     IPX_TYPE_ECHO, data, strlen(data)) == 0)
            terminal_writestring("IPX: datagram sent\n");
        else
            terminal_writestring("IPX: send failed\n");
    } else {
        terminal_writestring("ipx: unknown subcommand (try: send)\n");
    }
}
