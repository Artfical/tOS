#ifndef CMOS_H
#define CMOS_H
#include <stdint.h>
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71
#define CMOS_SECOND 0x00
#define CMOS_MINUTE 0x02
#define CMOS_HOUR 0x04
#define CMOS_DAY 0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR 0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_CENTURY 0x32
#define CMOS_NMI_DISABLE 0x80
typedef struct {
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
} cmos_time_t;
uint8_t cmos_read(uint8_t reg);
void cmos_write(uint8_t reg, uint8_t val);
int cmos_get_time(cmos_time_t *time);
#endif
