#include "bonding.h"
#include "string.h"
#include "terminal.h"
#include "memory.h"

/*
 * Link Aggregation / Bonding — failover (active-backup) mode.
 *
 * With a single NIC driver the bond exists as a logical layer:
 *   - TX goes through the active slave (the real nic_send).
 *   - On failover, active_slave advances to the next registered slave.
 *   - balance mode advances active_slave on each TX call (round-robin).
 *
 * To use with multiple NICs, the caller would swap nic_send to point at
 * whichever slave's send function is currently active.
 */

static bond_t bonds[BOND_MAX];
static int    bond_count = 0;

static bond_t *find_bond(const char *name)
{
    for (int i = 0; i < bond_count; i++)
        if (bonds[i].active && strncmp(bonds[i].name, name, BOND_NAME_LEN) == 0)
            return &bonds[i];
    return 0;
}

static void print_uint(int n)
{
    char buf[12]; int i = 11; buf[11] = '\0';
    if (n == 0) { terminal_putchar('0'); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

int bond_create(const char *name, bond_mode_t mode)
{
    if (find_bond(name)) return -1;
    if (bond_count >= BOND_MAX) return -1;
    bond_t *b = &bonds[bond_count++];
    memset(b, 0, sizeof(*b));
    strncpy(b->name, name, BOND_NAME_LEN - 1);
    b->mode   = mode;
    b->active = 1;
    return 0;
}

int bond_destroy(const char *name)
{
    bond_t *b = find_bond(name);
    if (!b) return -1;
    b->active = 0;
    return 0;
}

int bond_add_slave(const char *bondname, const char *ifname)
{
    bond_t *b = find_bond(bondname);
    if (!b) return -1;
    if (b->slave_count >= BOND_SLAVE_MAX) return -1;
    for (int i = 0; i < b->slave_count; i++)
        if (strncmp(b->slaves[i], ifname, BOND_NAME_LEN) == 0) return 0;
    strncpy(b->slaves[b->slave_count++], ifname, BOND_NAME_LEN - 1);
    return 0;
}

int bond_remove_slave(const char *bondname, const char *ifname)
{
    bond_t *b = find_bond(bondname);
    if (!b) return -1;
    for (int i = 0; i < b->slave_count; i++) {
        if (strncmp(b->slaves[i], ifname, BOND_NAME_LEN) == 0) {
            for (int j = i; j < b->slave_count - 1; j++)
                memcpy(b->slaves[j], b->slaves[j + 1], BOND_NAME_LEN);
            b->slave_count--;
            if (b->active_slave >= b->slave_count && b->slave_count > 0)
                b->active_slave = 0;
            return 0;
        }
    }
    return -1;
}

int bond_failover(const char *bondname)
{
    bond_t *b = find_bond(bondname);
    if (!b || b->slave_count < 2) return -1;
    b->active_slave = (b->active_slave + 1) % b->slave_count;
    terminal_writestring("bond ");
    terminal_writestring(b->name);
    terminal_writestring(": failover -> ");
    terminal_writestring(b->slaves[b->active_slave]);
    terminal_putchar('\n');
    return 0;
}

const char *bond_active_if(const char *bondname)
{
    bond_t *b = find_bond(bondname);
    if (!b || b->slave_count == 0) return 0;
    return b->slaves[b->active_slave];
}

void bond_list(void)
{
    if (bond_count == 0) {
        terminal_writestring("bond: no bonds configured\n");
        return;
    }
    for (int i = 0; i < bond_count; i++) {
        bond_t *b = &bonds[i];
        if (!b->active) continue;
        terminal_writestring("bond ");
        terminal_writestring(b->name);
        terminal_writestring("  mode=");
        terminal_writestring(b->mode == BOND_MODE_FAILOVER ? "failover" : "balance");
        terminal_writestring("  slaves:");
        if (b->slave_count == 0) {
            terminal_writestring(" (none)");
        } else {
            for (int j = 0; j < b->slave_count; j++) {
                terminal_putchar(' ');
                if (j == b->active_slave) terminal_putchar('[');
                terminal_writestring(b->slaves[j]);
                if (j == b->active_slave) terminal_putchar(']');
            }
        }
        terminal_writestring("  active_slave=");
        print_uint(b->active_slave);
        terminal_putchar('\n');
    }
}
