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

uint32_t debugmon_uptime_ms(void)
{
    return tick_count * MS_PER_TICK;
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
