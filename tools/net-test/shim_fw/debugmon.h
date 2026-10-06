#include <stdint.h>
extern uint32_t fake_now_ms;
static inline uint32_t debugmon_uptime_ms(void) { return fake_now_ms; }
