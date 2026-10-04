#ifndef SHIM_BLOCKDEV_H
#define SHIM_BLOCKDEV_H
#include <stdint.h>
typedef struct blockdev {
    int used;
    uint32_t sector_size;
    uint64_t total_sectors;
    int fd;
    int fail_after;   /* fault injection: fail every write after N writes (-1 = never) */
    long writes;
} blockdev_t;
int blockdev_read_bytes(blockdev_t *bd, uint64_t off, uint32_t len, void *buf);
int blockdev_write_bytes(blockdev_t *bd, uint64_t off, uint32_t len, const void *buf);
int blockdev_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf);
int blockdev_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf);
#endif
