#ifndef PAT_H
#define PAT_H
#include <stdint.h>
#define PAT_ENTRY_WB 6
#define PAT_ENTRY_WT 4
#define PAT_ENTRY_UC 0
#define PAT_ENTRY_UCM 1
#define PAT_ENTRY_WC 1
#define PAT_ENTRY_WP 5
typedef struct {
    int present;
    uint64_t pat_value;
} pat_info_t;
int pat_init(pat_info_t *info);
void pat_set_entry(int entry, int type);
#endif
