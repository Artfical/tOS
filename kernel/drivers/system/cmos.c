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
/* One pass over the clock registers. */
static void cmos_read_once(cmos_time_t *time)
{
    /* The chip updates its registers once a second; reading while the
     * update-in-progress flag (status A bit 7) is set can return a torn
     * mix of old and new fields (e.g. 12:59:59 -> 13:59:59 or 12:00:00). */
    for (int spin = 0; spin < 100000 && (cmos_read(CMOS_STATUS_A) & 0x80); spin++) { }

    uint8_t status_b = cmos_read(CMOS_STATUS_B);
    int is_bcd = !(status_b & 0x04);
    int is_24h = (status_b & 0x02) != 0;
    uint8_t sec = cmos_read(CMOS_SECOND);
    uint8_t min = cmos_read(CMOS_MINUTE);
    uint8_t hour = cmos_read(CMOS_HOUR);
    uint8_t day = cmos_read(CMOS_DAY);
    uint8_t mon = cmos_read(CMOS_MONTH);
    uint8_t year = cmos_read(CMOS_YEAR);
    uint8_t century = cmos_read(CMOS_CENTURY);

    /* In 12-hour mode bit 7 of the hour is the PM flag, not part of the value. */
    int pm = 0;
    if (!is_24h) { pm = (hour & 0x80) != 0; hour &= 0x7F; }

    int c;
    if (is_bcd) {
        time->second = bcd_to_bin(sec);
        time->minute = bcd_to_bin(min);
        time->hour = bcd_to_bin(hour);
        time->day = bcd_to_bin(day);
        time->month = bcd_to_bin(mon);
        c = bcd_to_bin(century);
        time->year = bcd_to_bin(year);
    } else {
        time->second = sec;
        time->minute = min;
        time->hour = hour;
        time->day = day;
        time->month = mon;
        c = century;
        time->year = year;
    }
    if (!is_24h) {
        if (time->hour == 12) time->hour = 0;
        if (pm) time->hour += 12;
    }
    /* Register 0x32 is only a century register on some chipsets; elsewhere it
     * reads 0 (the year then came out as 26 instead of 2026). Accept it only
     * when plausible, otherwise window the two-digit year: 70-99 -> 19xx. */
    if (c >= 19 && c <= 21) time->year += c * 100;
    else time->year += (time->year >= 70) ? 1900 : 2000;
}

int cmos_get_time(cmos_time_t *time)
{
    /* Read until two consecutive passes agree so a rollover in the middle of
     * a pass cannot slip through. */
    cmos_time_t a, b;
    cmos_read_once(&a);
    for (int tries = 0; tries < 5; tries++) {
        cmos_read_once(&b);
        if (a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year) break;
        a = b;
    }
    *time = a;
    return 0;
}
