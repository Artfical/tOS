#include "klog.h"
#include "string.h"
#include "debugmon.h"

#define KLOG_SIZE 4096
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

    debugmon_send_log(s);
}

const char *klog_get(int *len)
{
    if (len) *len = pos;
    return buf;
}
