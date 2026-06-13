#ifndef DEBUGMON_H
#define DEBUGMON_H

#include <stdint.h>
#include "isr.h"

#define DEBUGMON_REGDUMP_REQUEST 0x52  /* 'R' */

void debugmon_init(void);
void debugmon_send_log(const char *msg);
void debugmon_send_irq(uint8_t irq_number, uint32_t handler_addr);
void debugmon_send_regdump(registers_t *regs);
void debugmon_send_panic(uint32_t error_code, uint32_t eip, const char *msg);
void debugmon_send_memdump(uint32_t address, const uint8_t *data, uint16_t len);

/* Drains pending serial RX bytes; returns 1 if a regdump request (0x52)
 * was seen since the last call, 0 otherwise. */
int debugmon_poll_regdump_request(void);

#endif
