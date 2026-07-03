#include "debugmon.h"
#include "serial.h"
#include "string.h"

/* PIT is programmed for ~100Hz, so each tick is ~10ms. */
#define MS_PER_TICK 10

static volatile uint32_t tick_count = 0;

void debugmon_tick(void)
{
    tick_count++;
}

/* -----------------------------------------------------------------------
 * TSC-based wall clock, used once calibrated.
 *
 * tick_count (above) is driven by IRQ0 through kernel.c's original
 * wiring, but the scheduler later reinstalls its own IDT gate 32
 * handler for task switching, and — because task_yield() re-enters the
 * *same* vector via a software "int $32" — reliably telling a real PIT
 * tick apart from a software self-yield turned out to be fragile across
 * hypervisors (a PIC In-Service-Register check that worked under QEMU
 * did not hold up under VirtualBox). The TSC doesn't have this problem
 * at all: it's a free-running hardware cycle counter untouched by any
 * software interrupt, so once we know its frequency, elapsed wall-clock
 * time is just arithmetic — no interrupt-source ambiguity possible.
 * ----------------------------------------------------------------------- */
static uint64_t tsc_per_ms = 0;
static uint64_t calib_tsc0 = 0;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Must be called after interrupts are enabled but before the scheduler
 * takes over IDT gate 32, so tick_count is still being driven by a
 * genuine, unambiguous hardware IRQ0. Busy-waits for ~200ms of real
 * ticks to get a stable calibration. */
void debugmon_calibrate_tsc(void)
{
    while (tick_count == 0) { }
    uint32_t start_ticks = tick_count;
    uint64_t start_tsc = rdtsc();

    while (tick_count - start_ticks < 20) { } /* ~200ms at 100Hz */

    uint32_t elapsed_ticks = tick_count - start_ticks;
    uint64_t elapsed_tsc = rdtsc() - start_tsc;
    uint32_t elapsed_ms = elapsed_ticks * MS_PER_TICK;

    tsc_per_ms = elapsed_ms ? (elapsed_tsc / elapsed_ms) : 0;
    calib_tsc0 = rdtsc();
}

uint32_t debugmon_uptime_ms(void)
{
    if (tsc_per_ms == 0)
        return tick_count * MS_PER_TICK; /* pre-calibration fallback */
    return (uint32_t)((rdtsc() - calib_tsc0) / tsc_per_ms);
}

static int append_str(char *buf, int pos, int cap, const char *s)
{
    while (*s && pos < cap)
        buf[pos++] = *s++;
    return pos;
}

static int append_uint(char *buf, int pos, int cap, uint32_t value)
{
    char digits[10];
    int n = 0;

    if (value == 0) {
        if (pos < cap) buf[pos++] = '0';
        return pos;
    }

    while (value > 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && pos < cap)
        buf[pos++] = digits[--n];

    return pos;
}

static const char *detect_level(const char *msg)
{
    if (strstr(msg, "PANIC") || strstr(msg, "panic"))
        return "PANIC";
    if (strstr(msg, "[ERROR]") || strstr(msg, "ERROR"))
        return "ERROR";
    if (strstr(msg, "[WARN]") || strstr(msg, "WARNING"))
        return "WARNING";
    return "INFO";
}

void debugmon_log_line(const char *msg)
{
    char line[256];
    int pos = 0;
    int len = (int)strlen(msg);

    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        len--;

    if (len == 0)
        return;

    pos = append_str(line, pos, (int)sizeof(line) - 2, "[");
    pos = append_uint(line, pos, (int)sizeof(line) - 2, debugmon_uptime_ms());
    pos = append_str(line, pos, (int)sizeof(line) - 2, "] [");
    pos = append_str(line, pos, (int)sizeof(line) - 2, detect_level(msg));
    pos = append_str(line, pos, (int)sizeof(line) - 2, "] [KERNEL] ");

    for (int i = 0; i < len && pos < (int)sizeof(line) - 2; i++)
        line[pos++] = msg[i];

    line[pos++] = '\n';
    line[pos] = '\0';

    serial_write(line);
}
