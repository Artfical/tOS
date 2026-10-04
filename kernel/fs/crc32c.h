#ifndef CRC32C_H
#define CRC32C_H

/* CRC-32C (Castagnoli), as used by btrfs, ext4 metadata_csum and XFS v5.
 * crc32c_update() does NOT invert the seed/result: callers pass ~0 and
 * invert themselves where the on-disk format requires it. */

#include <stdint.h>
#include <stddef.h>

static inline uint32_t crc32c_update(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

#endif
