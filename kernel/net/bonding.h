#ifndef BONDING_H
#define BONDING_H

#include <stdint.h>

#define BOND_MAX        4
#define BOND_SLAVE_MAX  4
#define BOND_NAME_LEN   8

typedef enum {
    BOND_MODE_FAILOVER = 0, /* active-backup: only one slave active at a time */
    BOND_MODE_BALANCE  = 1, /* round-robin TX (receive always on active) */
} bond_mode_t;

typedef struct {
    char       name[BOND_NAME_LEN];
    char       slaves[BOND_SLAVE_MAX][BOND_NAME_LEN];
    int        slave_count;
    int        active_slave; /* index into slaves[] for current TX */
    bond_mode_t mode;
    int        active;
} bond_t;

int  bond_create(const char *name, bond_mode_t mode);
int  bond_destroy(const char *name);
int  bond_add_slave(const char *bondname, const char *ifname);
int  bond_remove_slave(const char *bondname, const char *ifname);
/* Trigger manual failover to the next slave (failover mode only) */
int  bond_failover(const char *bondname);
void bond_list(void);

/* Return the name of the currently active slave for a bond, or NULL */
const char *bond_active_if(const char *bondname);

#endif
