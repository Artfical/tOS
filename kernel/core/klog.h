#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

void klog_init(void);
void klog_write(const char *s);
const char *klog_get(int *len);

/* Appends `label` followed by a classic offset/hex/ASCII dump of
 * `data` (16 bytes per line) to the kernel log, in the same format
 * the `hexdump` shell command uses for files. */
void klog_write_hex(const char *label, const uint8_t *data, int len);

#endif
