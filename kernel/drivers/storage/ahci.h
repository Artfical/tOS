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
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
} __attribute__((packed)) ahci_port_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc_flags;
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1];
} __attribute__((packed)) ahci_cmd_tbl_t;

typedef struct {
    uint32_t base;
    int port_count;
    ahci_port_t *ports[AHCI_MAX_PORTS];
    ahci_cmd_header_t *cmd_list[AHCI_MAX_PORTS];
    ahci_cmd_tbl_t *cmd_tbl[AHCI_MAX_PORTS];
    uint8_t *fis_base[AHCI_MAX_PORTS];
    uint64_t sectors[AHCI_MAX_PORTS];
} ahci_hba_t;

int ahci_init(ahci_hba_t *hba);
int ahci_read(ahci_hba_t *hba, int port, uint64_t lba, int count, void *buf);
int ahci_write(ahci_hba_t *hba, int port, uint64_t lba, int count, const void *buf);
#endif
