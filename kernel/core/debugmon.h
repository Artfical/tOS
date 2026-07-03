#ifndef DEBUGMON_H
#define DEBUGMON_H

#include <stdint.h>

/* Called once per timer IRQ (IRQ0) to advance the uptime counter. */
void debugmon_tick(void);

/* Approximate uptime in milliseconds since boot. */
uint32_t debugmon_uptime_ms(void);

/* Call once after interrupts are enabled but before scheduler_init()
 * takes over IDT gate 32 — calibrates the TSC against ~200ms of real
 * PIT ticks so debugmon_uptime_ms() keeps working correctly (and can't
 * be sped up by task_yield()) even after the scheduler starts routing
 * software self-yields through the same interrupt vector. */
void debugmon_calibrate_tsc(void);

/* Formats msg as "[uptime_ms] [LEVEL] [KERNEL] msg" and writes it to the
 * serial port as a plain text line, matching tOS_monitor's .log format. */
void debugmon_log_line(const char *msg);

#endif
