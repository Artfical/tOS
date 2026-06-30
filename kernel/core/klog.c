#include "klog.h"
#include "string.h"
#include "debugmon.h"

/* Bumped from 4096: hex dumps (klog_write_hex(), used to log disk
 * format/mount operations) are a lot denser than plain boot messages
 * and would otherwise truncate the log almost immediately. */
#define KLOG_SIZE 16384
static char buf[KLOG_SIZE];
static int pos;

void klog_init(void)
{
    pos = 0;
    buf[0] = '\0';
}

void klog_write(const char *s)
{
    const char *p = s;
    while (*p && pos < KLOG_SIZE - 1)
        buf[pos++] = *p++;
    buf[pos] = '\0';

    debugmon_log_line(s);
}

const char *klog_get(int *len)
{
    if (len) *len = pos;
    return buf;
}

void klog_write_hex(const char *label, const uint8_t *data, int len)
{
    klog_write(label);
    klog_write("\n");

    static const char hex[] = "0123456789ABCDEF";
    char line[80];
    for (int off = 0; off < len; off += 16) {
        int k = 0;
        line[k++] = hex[(off >> 12) & 0xF];
        line[k++] = hex[(off >> 8) & 0xF];
        line[k++] = hex[(off >> 4) & 0xF];
        line[k++] = hex[off & 0xF];
        line[k++] = ' ';
        line[k++] = ' ';
        for (int i = 0; i < 16; i++) {
            if (off + i < len) {
                uint8_t b = data[off + i];
                line[k++] = hex[(b >> 4) & 0xF];
                line[k++] = hex[b & 0xF];
            } else {
                line[k++] = ' ';
                line[k++] = ' ';
            }
            line[k++] = ' ';
            if (i == 7) line[k++] = ' ';
        }
        line[k++] = '|';
        for (int i = 0; i < 16 && off + i < len; i++) {
            uint8_t b = data[off + i];
            line[k++] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        line[k++] = '|';
        line[k++] = '\n';
        line[k] = 0;
        klog_write(line);
    }
}
