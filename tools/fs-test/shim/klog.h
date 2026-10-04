#include <stdio.h>
static inline void klog_write(const char *s) { fputs(s, stderr); }
