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
