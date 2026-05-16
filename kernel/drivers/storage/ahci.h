#ifndef AHCI_H
#define AHCI_H
#include <stdint.h>
#define AHCI_BASE 0
#define AHCI_MAX_PORTS 32
#define AHCI_CMD_READ_DMA 0x25
#define AHCI_CMD_WRITE_DMA 0x35
#define AHCI_SIG_ATA 0x00000101
#define AHCI_SIG_ATAPI 0xEB140101
typedef struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
} __attribute__((packed)) ahci_port_t;
typedef struct {
    uint32_t base;
    int port_count;
    ahci_port_t *ports[AHCI_MAX_PORTS];
} ahci_hba_t;
int ahci_init(ahci_hba_t *hba);
int ahci_read(ahci_hba_t *hba, int port, uint64_t lba, int count, void *buf);
int ahci_write(ahci_hba_t *hba, int port, uint64_t lba, int count, const void *buf);
#endif
