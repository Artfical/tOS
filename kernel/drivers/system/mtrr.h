#ifndef MTRR_H
#define MTRR_H
#include <stdint.h>
#define MTRR_TYPE_UNCACHEABLE 0
#define MTRR_TYPE_WRITECOMBINING 1
#define MTRR_TYPE_WRITETHROUGH 4
#define MTRR_TYPE_WRITEPROTECT 5
#define MTRR_TYPE_WRITEBACK 6
#define MTRR_NUM_FIXED 11
typedef struct {
    int present;
    int var_count;
} mtrr_info_t;
int mtrr_init(mtrr_info_t *info);
int mtrr_set_var(int index, uint64_t base, uint64_t mask, int type);
#endif
