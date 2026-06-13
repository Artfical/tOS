#ifndef DEBUGMON_H
#define DEBUGMON_H

#include <stdint.h>

/* Called once per timer IRQ (IRQ0) to advance the uptime counter. */
void debugmon_tick(void);

/* Approximate uptime in milliseconds since boot. */
uint32_t debugmon_uptime_ms(void);

/* Formats msg as "[uptime_ms] [LEVEL] [KERNEL] msg" and writes it to the
 * serial port as a plain text line, matching tOS_monitor's .log format. */
void debugmon_log_line(const char *msg);

#endif
