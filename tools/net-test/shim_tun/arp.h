#ifndef SHIM_ARP_H
#define SHIM_ARP_H
static inline int arp_resolve(unsigned ip, unsigned char *mac) { (void)ip; (void)mac; return 0; }
#endif
