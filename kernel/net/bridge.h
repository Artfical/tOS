#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

#define BRIDGE_MAX      4
#define BRIDGE_IF_MAX   4
#define BRIDGE_NAME_LEN 8

typedef struct {
    char     name[BRIDGE_NAME_LEN];
    char     ifaces[BRIDGE_IF_MAX][BRIDGE_NAME_LEN]; /* member interface names */
    int      iface_count;
    int      active;
    uint8_t  mac[6];  /* bridge MAC = first member's MAC */
} bridge_t;

int  bridge_create(const char *name);
int  bridge_destroy(const char *name);
int  bridge_add_if(const char *brname, const char *ifname);
int  bridge_remove_if(const char *brname, const char *ifname);
void bridge_list(void);

/*
 * Called from net_poll() for every received frame.
 * Returns 1 if the bridge absorbed the frame (don't process further),
 * 0 if normal processing should continue.
 *
 * In this minimal implementation the bridge just logs and passes through;
 * a real L2 bridge would consult an FDB and forward out other ports.
 */
int bridge_rx(const char *in_if, uint8_t *frame, int len);

#endif
