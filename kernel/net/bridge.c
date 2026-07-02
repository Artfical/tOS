#include "bridge.h"
#include "net.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

/*
 * Minimal software bridge (br0-style).
 * Maintains a bridge table and an FDB (Forwarding DataBase) of 16 entries.
 * With a single NIC there is nowhere to forward, so bridge_rx() just
 * updates the FDB and returns 0 (let the normal IP stack handle it).
 * When multiple NICs are present the caller can extend bridge_rx() to
 * call nic_send() on each non-ingress port.
 */

#define FDB_SIZE 16

typedef struct {
    uint8_t mac[6];
    char    port[BRIDGE_NAME_LEN];
    int     valid;
} fdb_entry_t;

static bridge_t   bridges[BRIDGE_MAX];
static int        bridge_count = 0;
static fdb_entry_t fdb[FDB_SIZE];

/* -----------------------------------------------------------------------
 * FDB helpers
 * ----------------------------------------------------------------------- */
static void fdb_learn(const uint8_t *mac, const char *port)
{
    /* update existing */
    for (int i = 0; i < FDB_SIZE; i++) {
        if (fdb[i].valid && memcmp(fdb[i].mac, mac, 6) == 0) {
            strncpy(fdb[i].port, port, BRIDGE_NAME_LEN - 1);
            return;
        }
    }
    /* find empty slot */
    for (int i = 0; i < FDB_SIZE; i++) {
        if (!fdb[i].valid) {
            memcpy(fdb[i].mac, mac, 6);
            strncpy(fdb[i].port, port, BRIDGE_NAME_LEN - 1);
            fdb[i].valid = 1;
            return;
        }
    }
    /* FDB full: overwrite slot 0 (simple eviction) */
    memcpy(fdb[0].mac, mac, 6);
    strncpy(fdb[0].port, port, BRIDGE_NAME_LEN - 1);
}

static bridge_t *find_bridge_by_name(const char *name)
{
    for (int i = 0; i < bridge_count; i++)
        if (bridges[i].active && strncmp(bridges[i].name, name, BRIDGE_NAME_LEN) == 0)
            return &bridges[i];
    return 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
int bridge_create(const char *name)
{
    if (find_bridge_by_name(name)) return -1; /* already exists */
    if (bridge_count >= BRIDGE_MAX) return -1;
    bridge_t *br = &bridges[bridge_count++];
    memset(br, 0, sizeof(*br));
    strncpy(br->name, name, BRIDGE_NAME_LEN - 1);
    br->active = 1;
    return 0;
}

int bridge_destroy(const char *name)
{
    bridge_t *br = find_bridge_by_name(name);
    if (!br) return -1;
    br->active = 0;
    return 0;
}

int bridge_add_if(const char *brname, const char *ifname)
{
    bridge_t *br = find_bridge_by_name(brname);
    if (!br) return -1;
    if (br->iface_count >= BRIDGE_IF_MAX) return -1;
    for (int i = 0; i < br->iface_count; i++)
        if (strncmp(br->ifaces[i], ifname, BRIDGE_NAME_LEN) == 0) return 0;
    strncpy(br->ifaces[br->iface_count++], ifname, BRIDGE_NAME_LEN - 1);
    /* inherit MAC from first member */
    if (br->iface_count == 1)
        memcpy(br->mac, net_mac, 6);
    return 0;
}

int bridge_remove_if(const char *brname, const char *ifname)
{
    bridge_t *br = find_bridge_by_name(brname);
    if (!br) return -1;
    for (int i = 0; i < br->iface_count; i++) {
        if (strncmp(br->ifaces[i], ifname, BRIDGE_NAME_LEN) == 0) {
            br->ifaces[i][0] = '\0';
            /* compact */
            for (int j = i; j < br->iface_count - 1; j++)
                memcpy(br->ifaces[j], br->ifaces[j + 1], BRIDGE_NAME_LEN);
            br->iface_count--;
            return 0;
        }
    }
    return -1;
}

void bridge_list(void)
{
    if (bridge_count == 0) {
        terminal_writestring("bridge: no bridges configured\n");
        return;
    }
    for (int i = 0; i < bridge_count; i++) {
        bridge_t *br = &bridges[i];
        if (!br->active) continue;
        terminal_writestring("bridge ");
        terminal_writestring(br->name);
        terminal_writestring("  members:");
        if (br->iface_count == 0) {
            terminal_writestring(" (none)");
        } else {
            for (int j = 0; j < br->iface_count; j++) {
                terminal_putchar(' ');
                terminal_writestring(br->ifaces[j]);
            }
        }
        terminal_putchar('\n');
    }
    terminal_writestring("FDB:\n");
    int any = 0;
    for (int i = 0; i < FDB_SIZE; i++) {
        if (!fdb[i].valid) continue;
        any = 1;
        terminal_writestring("  ");
        for (int b = 0; b < 6; b++) {
            static const char hex[] = "0123456789abcdef";
            terminal_putchar(hex[(fdb[i].mac[b] >> 4) & 0xF]);
            terminal_putchar(hex[fdb[i].mac[b] & 0xF]);
            if (b < 5) terminal_putchar(':');
        }
        terminal_writestring(" -> ");
        terminal_writestring(fdb[i].port);
        terminal_putchar('\n');
    }
    if (!any) terminal_writestring("  (empty)\n");
}

int bridge_rx(const char *in_if, uint8_t *frame, int len)
{
    if (len < 12) return 0;
    /* learn source MAC */
    fdb_learn(frame + 6, in_if);

    /* Check if this frame is destined for the bridge's own MAC —
       if so, pass to normal IP stack (return 0).
       A multicast/broadcast is also passed up. */
    uint8_t *dst = frame;
    if (dst[0] & 0x01) return 0; /* broadcast/multicast — pass up */
    if (memcmp(dst, net_mac, 6) == 0) return 0; /* unicast to us */

    /* Unknown unicast: in a real bridge we'd forward out all other ports.
       With a single NIC we just drop. */
    return 1;
}
