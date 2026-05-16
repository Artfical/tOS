#include "cmos.h"
#include "io.h"
uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}
void cmos_write(uint8_t reg, uint8_t val)
{
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, val);
}
static int bcd_to_bin(uint8_t bcd)
{
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}
int cmos_get_time(cmos_time_t *time)
{
    uint8_t status_b = cmos_read(CMOS_STATUS_B);
    int is_bcd = !(status_b & 0x04);
    uint8_t sec = cmos_read(CMOS_SECOND);
    uint8_t min = cmos_read(CMOS_MINUTE);
    uint8_t hour = cmos_read(CMOS_HOUR);
    uint8_t day = cmos_read(CMOS_DAY);
    uint8_t mon = cmos_read(CMOS_MONTH);
    uint8_t year = cmos_read(CMOS_YEAR);
    uint8_t century = cmos_read(CMOS_CENTURY);
    if (is_bcd) {
        time->second = bcd_to_bin(sec);
        time->minute = bcd_to_bin(min);
        time->hour = bcd_to_bin(hour);
        time->day = bcd_to_bin(day);
        time->month = bcd_to_bin(mon);
        time->year = bcd_to_bin(century) * 100 + bcd_to_bin(year);
    } else {
        time->second = sec;
        time->minute = min;
        time->hour = hour;
        time->day = day;
        time->month = mon;
        time->year = century * 100 + year;
    }
    return 0;
}
