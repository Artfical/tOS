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

/* Diagnostic accessor — TSC cycles per millisecond found by calibration
 * (0 if calibration hasn't run / failed). */
uint32_t debugmon_get_tsc_per_ms(void);

/* Diagnostic accessor — raw hardware tick count (only meaningful before
 * scheduler_init() reinstalls IDT gate 32). */
uint32_t debugmon_get_tick_count(void);

/* Formats msg as "[uptime_ms] [LEVEL] [KERNEL] msg" and writes it to the
 * serial port as a plain text line, matching tOS_monitor's .log format. */
void debugmon_log_line(const char *msg);

#endif
