#ifndef HPET_H
#define HPET_H
#include <stdint.h>
#define HPET_BASE 0xFED00000
#define HPET_GCAP_ID 0x000
#define HPET_GEN_CFG 0x010
#define HPET_GEN_INT_STS 0x020
#define HPET_MAIN_CNT 0x0F0
#define HPET_TIMER0_CFG 0x100
#define HPET_TIMER0_COMP 0x108
typedef struct {
    int present;
    uint32_t base;
    uint64_t frequency;
} hpet_timer_t;
int hpet_init(hpet_timer_t *hpet);
void hpet_sleep(hpet_timer_t *hpet, uint64_t ms);
uint64_t hpet_read_counter(hpet_timer_t *hpet);
#endif
