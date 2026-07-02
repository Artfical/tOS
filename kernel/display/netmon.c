#include "netmon.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "net.h"
#include "nic.h"
#include "tcp.h"
#include "udp.h"
#include "sctp.h"
#include "route.h"
#include "fw.h"

#define NM_COLS  79
#define NM_ROWS  22
#define CONTENT_Y0 3          /* first content row */
#define CONTENT_H  (NM_ROWS - CONTENT_Y0 - 1)  /* 18 rows */
#define STATUS_Y   (NM_ROWS - 1)

#define TAB_IFACES  0
#define TAB_CONNS   1
#define TAB_ROUTES  2
#define TAB_FIREWALL 3
#define TAB_COUNT   4

static int  g_tab    = TAB_IFACES;
static int  g_scroll = 0;

/* Live-traffic snapshot */
static uint32_t g_prev_rx = 0, g_prev_tx = 0;
static uint32_t g_rx_rate = 0, g_tx_rate = 0;
static uint32_t g_last_rate_tick = 0;

/* ---- Tiny formatting helpers ---- */
static uint8_t nm_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void put_char(int x, int y, char c, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar(c);
}

/* Fill a full NM_COLS-wide row with spaces */
static void fill_row(int y, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos(0, (size_t)y);
    int i;
    for (i = 0; i < NM_COLS; i++) terminal_putchar(' ');
}

/* Write a line into a fixed NM_COLS-char row, padding with spaces */
static void put_line(int y, const char *s, uint8_t color)
{
    fill_row(y, color);
    put_str(0, y, s, color);
}

/* Simple uint32 → string, returns length */
static int fmt_u32(char *buf, uint32_t v)
{
    char tmp[12]; int n = 0;
    if (v == 0) { buf[0]='0'; return 1; }
    while (v && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int i; for (i = 0; i < n; i++) buf[i] = tmp[n-1-i];
    return n;
}

/* Append uint32 to char buf at pos *k, return new pos */
static int app_u32(char *buf, int k, uint32_t v)
{
    char tmp[12]; int n = fmt_u32(tmp, v);
    int i; for (i = 0; i < n && k < NM_COLS; i++) buf[k++] = tmp[i];
    return k;
}

static int app_str(char *buf, int k, const char *s)
{
    while (*s && k < NM_COLS) buf[k++] = *s++;
    return k;
}

/* Format IPv4 x.x.x.x → buf at *k */
static int app_ip(char *buf, int k, uint32_t ip)
{
    k = app_u32(buf, k, ip & 0xFF);      buf[k++]='.';
    k = app_u32(buf, k, (ip>>8)&0xFF);   buf[k++]='.';
    k = app_u32(buf, k, (ip>>16)&0xFF);  buf[k++]='.';
    k = app_u32(buf, k, (ip>>24)&0xFF);
    return k;
}

static void pad_to(char *buf, int *k, int col)
{
    while (*k < col && *k < NM_COLS) buf[(*k)++] = ' ';
}

/* ---- Tab bar ---- */
static int tab_x0[TAB_COUNT], tab_x1[TAB_COUNT];

static void draw_tabs(void)
{
    static const char *names[TAB_COUNT] = {
        "Interfaces", "Connections", "Routes", "Firewall"
    };
    uint8_t bg_active   = nm_color(VGA_WHITE, VGA_BLUE);
    uint8_t bg_inactive = nm_color(VGA_BLACK, VGA_LIGHT_GREY);

    fill_row(1, bg_inactive);
    int x = 0;
    int i;
    for (i = 0; i < TAB_COUNT; i++) {
        const char *n = names[i];
        int len = (int)strlen(n) + 4; /* "[ name ]" */
        tab_x0[i] = x;
        tab_x1[i] = x + len - 1;
        uint8_t c = (i == g_tab) ? bg_active : bg_inactive;
        put_char(x, 1, '[', c); x++;
        put_char(x, 1, ' ', c); x++;
        put_str(x, 1, n, c);  x += (int)strlen(n);
        put_char(x, 1, ' ', c); x++;
        put_char(x, 1, ']', c); x++;
        if (x < NM_COLS) put_char(x, 1, ' ', bg_inactive);
        x++;
    }
    /* Key hint on right */
    const char *hint = " 1-4:Tab TAB:Next";
    int hx = NM_COLS - (int)strlen(hint);
    if (hx > x) put_str(hx, 1, hint, bg_inactive);
}

/* ---- Header bar (row 0) ---- */
static void draw_header(void)
{
    char line[NM_COLS + 1];
    int k = 0;
    k = app_str(line, k, " Network Monitor  NIC:");
    k = app_str(line, k, nic_driver_name);
    k = app_str(line, k, "  IP:");
    k = app_ip(line, k, net_ip);
    k = app_str(line, k, "  MAC:");
    const char *hex = "0123456789ABCDEF";
    int j;
    for (j = 0; j < 6 && k < NM_COLS-2; j++) {
        line[k++] = hex[(net_mac[j]>>4)&0xF];
        line[k++] = hex[net_mac[j]&0xF];
        if (j < 5) line[k++] = ':';
    }
    while (k < NM_COLS) line[k++] = ' ';
    line[NM_COLS] = 0;
    put_str(0, 0, line, nm_color(VGA_WHITE, VGA_BLUE));
}

/* ---- Column header (row 2) ---- */
static const char *col_headers[TAB_COUNT] = {
    " Interface   Driver        TX Pkts    TX Bytes   RX Pkts    RX Bytes",
    " Proto  State            Local Port  Remote IP            Remote Port",
    " Destination         Mask                 Gateway              Metric",
    " #   Proto  Src IP               Src Port  Dst IP               Dst Port  Action",
};

static void draw_col_header(void)
{
    put_line(2, col_headers[g_tab], nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
}

/* ---- Status bar (row STATUS_Y): live traffic ---- */
static void update_rate(void)
{
    uint32_t now = task_get_ticks();
    uint32_t elapsed = now - g_last_rate_tick;
    if (elapsed >= 100) { /* ~1 second */
        uint32_t rx_now = nic_rx_packets;
        uint32_t tx_now = nic_tx_packets;
        g_rx_rate = (elapsed > 0) ? ((rx_now - g_prev_rx) * 100) / elapsed : 0;
        g_tx_rate = (elapsed > 0) ? ((tx_now - g_prev_tx) * 100) / elapsed : 0;
        g_prev_rx = rx_now;
        g_prev_tx = tx_now;
        g_last_rate_tick = now;
    }
}

static void draw_status(void)
{
    char line[NM_COLS + 1];
    int k = 0;
    k = app_str(line, k, " RX:");
    k = app_u32(line, k, g_rx_rate); k = app_str(line, k, "pps  TX:");
    k = app_u32(line, k, g_tx_rate); k = app_str(line, k, "pps  |");

    /* ASCII bar: max 20 chars wide, scale = pps / 5 per bar */
    k = app_str(line, k, " RX:");
    uint32_t rx_bars = g_rx_rate / 5; if (rx_bars > 10) rx_bars = 10;
    uint32_t i;
    for (i = 0; i < rx_bars && k < NM_COLS; i++) line[k++] = '|';
    for (i = rx_bars; i < 10 && k < NM_COLS; i++) line[k++] = '.';
    k = app_str(line, k, " TX:");
    uint32_t tx_bars = g_tx_rate / 5; if (tx_bars > 10) tx_bars = 10;
    for (i = 0; i < tx_bars && k < NM_COLS; i++) line[k++] = '|';
    for (i = tx_bars; i < 10 && k < NM_COLS; i++) line[k++] = '.';
    k = app_str(line, k, "  Total RX:");
    k = app_u32(line, k, nic_rx_packets);
    k = app_str(line, k, " TX:");
    k = app_u32(line, k, nic_tx_packets);

    while (k < NM_COLS) line[k++] = ' ';
    line[NM_COLS] = 0;
    put_str(0, STATUS_Y, line, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
}

/* ---- Tab content drawing ---- */

static void draw_ifaces(void)
{
    char line[NM_COLS + 1];
    int y = CONTENT_Y0;
    /* Only one physical interface for now */
    {
        int k = 0;
        k = app_str(line, k, " eth0");
        pad_to(line, &k, 13);
        k = app_str(line, k, nic_driver_name);
        pad_to(line, &k, 27);
        k = app_u32(line, k, nic_tx_packets);
        pad_to(line, &k, 37);
        k = app_u32(line, k, nic_tx_bytes);
        pad_to(line, &k, 48);
        k = app_u32(line, k, nic_rx_packets);
        pad_to(line, &k, 58);
        k = app_u32(line, k, nic_rx_bytes);
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        if (y < STATUS_Y) {
            put_str(0, y, line, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
            y++;
        }
    }
    /* Show IP/GW/DNS info */
    if (y < STATUS_Y) {
        int k = 0;
        k = app_str(line, k, "   IPv4  : ");
        k = app_ip(line, k, net_ip);
        pad_to(line, &k, 27);
        k = app_str(line, k, "GW:");
        k = app_ip(line, k, net_gateway);
        pad_to(line, &k, 48);
        k = app_str(line, k, "DNS:");
        k = app_ip(line, k, net_dns);
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        put_str(0, y, line, nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
        y++;
    }
    if (y < STATUS_Y) {
        int k = 0;
        k = app_str(line, k, "   MAC   : ");
        const char *hex = "0123456789ABCDEF";
        int j;
        for (j = 0; j < 6 && k < NM_COLS; j++) {
            line[k++] = hex[(net_mac[j]>>4)&0xF];
            line[k++] = hex[net_mac[j]&0xF];
            if (j < 5 && k < NM_COLS) line[k++] = ':';
        }
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        put_str(0, y, line, nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
        y++;
    }
    /* Clear rest */
    while (y < STATUS_Y) {
        fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
        y++;
    }
}

static const char *tcp_state_name(int state)
{
    switch (state) {
        case 0:  return "CLOSED";
        case 1:  return "LISTEN";
        case 2:  return "SYN_SENT";
        case 3:  return "SYN_RCVD";
        case 4:  return "ESTABLISHED";
        case 5:  return "FIN_WAIT_1";
        case 6:  return "FIN_WAIT_2";
        case 7:  return "CLOSE_WAIT";
        case 8:  return "CLOSING";
        case 9:  return "LAST_ACK";
        case 10: return "TIME_WAIT";
        default: return "UNKNOWN";
    }
}

static const char *sctp_state_name(int state)
{
    switch (state) {
        case 0: return "CLOSED";
        case 1: return "COOKIE_WAIT";
        case 2: return "COOKIE_ECHO";
        case 3: return "ESTABLISHED";
        case 4: return "SHUTDOWN";
        default: return "?";
    }
}

static int total_conn_rows;

static void draw_conns(void)
{
    tcp_conn_info_t tc[16];
    udp_sock_info_t us[4];
    sctp_info_t si;
    int ntcp = tcp_get_connections(tc, 16);
    int nudp = udp_get_sockets(us, 4);
    int nsctp = sctp_get_info(&si);

    char rows[32][NM_COLS + 1];
    int nrows = 0;

    int i;
    for (i = 0; i < ntcp && nrows < 32; i++) {
        char *line = rows[nrows];
        int k = 0;
        k = app_str(line, k, " TCP");
        pad_to(line, &k, 8);
        const char *st = tcp_state_name(tc[i].state);
        k = app_str(line, k, st);
        pad_to(line, &k, 25);
        k = app_u32(line, k, tc[i].src_port);
        pad_to(line, &k, 37);
        k = app_ip(line, k, tc[i].dst_ip);
        pad_to(line, &k, 58);
        k = app_u32(line, k, tc[i].dst_port);
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        nrows++;
    }
    for (i = 0; i < nudp && nrows < 32; i++) {
        char *line = rows[nrows];
        int k = 0;
        k = app_str(line, k, " UDP");
        pad_to(line, &k, 8);
        k = app_str(line, k, us[i].has_data ? "RECV_PENDING" : "LISTEN");
        pad_to(line, &k, 25);
        k = app_u32(line, k, us[i].port);
        pad_to(line, &k, 37);
        k = app_str(line, k, "*");
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        nrows++;
    }
    if (nsctp && nrows < 32) {
        char *line = rows[nrows];
        int k = 0;
        k = app_str(line, k, " SCTP");
        pad_to(line, &k, 8);
        k = app_str(line, k, sctp_state_name(si.state));
        pad_to(line, &k, 25);
        k = app_u32(line, k, si.src_port);
        pad_to(line, &k, 37);
        k = app_ip(line, k, si.dst_ip);
        pad_to(line, &k, 58);
        k = app_u32(line, k, si.dst_port);
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        nrows++;
    }

    if (nrows == 0) {
        /* Show "No active connections" */
        int y = CONTENT_Y0;
        fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
        put_str(2, y, "No active connections.", nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
        y++;
        while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
        return;
    }

    if (g_scroll > nrows - CONTENT_H) g_scroll = nrows - CONTENT_H;
    if (g_scroll < 0) g_scroll = 0;

    int y = CONTENT_Y0;
    for (i = g_scroll; i < nrows && y < STATUS_Y; i++, y++) {
        put_str(0, y, rows[i], nm_color(VGA_BLACK, VGA_LIGHT_GREY));
    }
    while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
    total_conn_rows = nrows;
}

static int total_route_rows;

static void draw_routes(void)
{
    int count = route_get_count();
    char rows[32][NM_COLS + 1];
    int nrows = 0;
    int i;

    for (i = 0; i < count && nrows < 32; i++) {
        route_entry_t re;
        if (route_get_entry(i, &re) != 0) continue;
        if (!re.valid) continue;
        char *line = rows[nrows];
        int k = 0;
        line[k++] = ' ';
        k = app_ip(line, k, re.dst);
        pad_to(line, &k, 21);
        k = app_ip(line, k, re.mask);
        pad_to(line, &k, 42);
        if (re.gw == 0) {
            k = app_str(line, k, "direct");
        } else {
            k = app_ip(line, k, re.gw);
        }
        pad_to(line, &k, 63);
        k = app_u32(line, k, (uint32_t)re.metric);
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        nrows++;
    }

    if (nrows == 0) {
        int y = CONTENT_Y0;
        fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
        put_str(2, y, "Routing table is empty.", nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
        y++;
        while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
        total_route_rows = 0;
        return;
    }

    if (g_scroll > nrows - CONTENT_H) g_scroll = nrows - CONTENT_H;
    if (g_scroll < 0) g_scroll = 0;

    int y = CONTENT_Y0;
    for (i = g_scroll; i < nrows && y < STATUS_Y; i++, y++) {
        put_str(0, y, rows[i], nm_color(VGA_BLACK, VGA_LIGHT_GREY));
    }
    while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
    total_route_rows = nrows;
}

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
        case 6:   return "TCP";
        case 17:  return "UDP";
        case 132: return "SCTP";
        case 1:   return "ICMP";
        case 0:   return "*";
        default:  return "?";
    }
}

static const char *fw_action_name(uint8_t action)
{
    switch (action) {
        case 0: return "ACCEPT";
        case 1: return "DROP";
        case 2: return "REJECT";
        default: return "?";
    }
}

static int total_fw_rows;

static void draw_firewall(void)
{
    int count = fw_get_rule_count();
    char rows[32][NM_COLS + 1];
    int nrows = 0;
    int i;

    for (i = 0; i < count && nrows < 32; i++) {
        fw_rule_t fr;
        if (fw_get_rule(i, &fr) != 0) continue;
        if (!fr.valid) continue;
        char *line = rows[nrows];
        int k = 0;
        line[k++] = ' ';
        k = app_u32(line, k, (uint32_t)(nrows + 1));
        pad_to(line, &k, 5);
        k = app_str(line, k, proto_name(fr.proto));
        pad_to(line, &k, 10);
        k = app_ip(line, k, fr.src_ip);
        pad_to(line, &k, 32);
        if (fr.src_port) k = app_u32(line, k, fr.src_port);
        else k = app_str(line, k, "*");
        pad_to(line, &k, 42);
        k = app_ip(line, k, fr.dst_ip);
        pad_to(line, &k, 64);
        if (fr.dst_port) k = app_u32(line, k, fr.dst_port);
        else k = app_str(line, k, "*");
        pad_to(line, &k, 74);
        k = app_str(line, k, fw_action_name(fr.action));
        while (k < NM_COLS) line[k++] = ' ';
        line[NM_COLS] = 0;
        nrows++;
    }

    if (nrows == 0) {
        int y = CONTENT_Y0;
        fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
        put_str(2, y, "No firewall rules configured.", nm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
        y++;
        while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
        total_fw_rows = 0;
        return;
    }

    if (g_scroll > nrows - CONTENT_H) g_scroll = nrows - CONTENT_H;
    if (g_scroll < 0) g_scroll = 0;

    int y = CONTENT_Y0;
    for (i = g_scroll; i < nrows && y < STATUS_Y; i++, y++) {
        put_str(0, y, rows[i], nm_color(VGA_BLACK, VGA_LIGHT_GREY));
    }
    while (y < STATUS_Y) { fill_row(y, nm_color(VGA_BLACK, VGA_LIGHT_GREY)); y++; }
    total_fw_rows = nrows;
}

/* ---- Full redraw ---- */
static void redraw(void)
{
    update_rate();
    draw_header();
    draw_tabs();
    draw_col_header();
    switch (g_tab) {
        case TAB_IFACES:   draw_ifaces();   break;
        case TAB_CONNS:    draw_conns();    break;
        case TAB_ROUTES:   draw_routes();   break;
        case TAB_FIREWALL: draw_firewall(); break;
        default: break;
    }
    draw_status();
}

/* ---- Scroll helpers ---- */
static int max_rows_for_tab(void)
{
    switch (g_tab) {
        case TAB_CONNS:    return total_conn_rows;
        case TAB_ROUTES:   return total_route_rows;
        case TAB_FIREWALL: return total_fw_rows;
        default: return 1;
    }
}

static void scroll_down(void)
{
    int maxr = max_rows_for_tab();
    if (g_scroll + CONTENT_H < maxr) g_scroll++;
}

static void scroll_up(void)
{
    if (g_scroll > 0) g_scroll--;
}

/* ---- Main entry point ---- */
void netmon_run(void)
{
    g_tab    = TAB_IFACES;
    g_scroll = 0;
    g_prev_rx = nic_rx_packets;
    g_prev_tx = nic_tx_packets;
    g_rx_rate = 0; g_tx_rate = 0;
    g_last_rate_tick = task_get_ticks();
    total_conn_rows = 0; total_route_rows = 0; total_fw_rows = 0;

    terminal_clear();

    /* Pre-clear background */
    {
        int r;
        for (r = 0; r < NM_ROWS; r++)
            fill_row(r, nm_color(VGA_BLACK, VGA_LIGHT_GREY));
    }

    uint32_t last_redraw = 0;

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        /* Mouse click on tab bar */
        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == 1) {
                int t;
                for (t = 0; t < TAB_COUNT; t++) {
                    if (ccx >= tab_x0[t] && ccx <= tab_x1[t]) {
                        g_tab = t;
                        g_scroll = 0;
                        break;
                    }
                }
            } else if (ccy >= CONTENT_Y0 && ccy < STATUS_Y) {
                /* click in content — no action needed */
            }
            redraw();
            task_yield();
            continue;
        }

        /* Keyboard */
        int spec = keyboard_get_special();
        if (spec == 3) { scroll_up(); redraw(); task_yield(); continue; }   /* UP */
        if (spec == 4) { scroll_down(); redraw(); task_yield(); continue; } /* DOWN */

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c == '1') { g_tab = TAB_IFACES;   g_scroll = 0; }
            else if (c == '2') { g_tab = TAB_CONNS;    g_scroll = 0; }
            else if (c == '3') { g_tab = TAB_ROUTES;   g_scroll = 0; }
            else if (c == '4') { g_tab = TAB_FIREWALL; g_scroll = 0; }
            else if (c == '\t') {
                g_tab = (g_tab + 1) % TAB_COUNT;
                g_scroll = 0;
            }
            redraw();
            task_yield();
            continue;
        }

        /* Auto-refresh every ~50 ticks (0.5 s) */
        uint32_t now = task_get_ticks();
        if (now - last_redraw >= 50) {
            last_redraw = now;
            redraw();
        }

        task_yield();
    }
}
