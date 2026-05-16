#ifndef TSC_H
#define TSC_H
#include <stdint.h>
typedef struct {
    int present;
    uint64_t frequency;
} tsc_timer_t;
int tsc_init(tsc_timer_t *tsc);
uint64_t tsc_read(void);
void tsc_delay_us(tsc_timer_t *tsc, uint64_t us);
#endif
