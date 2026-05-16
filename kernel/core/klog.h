#ifndef KLOG_H
#define KLOG_H

void klog_init(void);
void klog_write(const char *s);
const char *klog_get(int *len);

#endif
