#ifndef TFIRE_TIME_H
#define TFIRE_TIME_H
/* what tFire's third-party libraries need of <time.h> (they get the time from tOS through their own hooks) */
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#endif
