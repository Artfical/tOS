#ifndef MICROPY_INCLUDED_TOS_MPHALPORT_H
#define MICROPY_INCLUDED_TOS_MPHALPORT_H

#include "py/mpconfig.h"
#include <stddef.h>

// Define pin API to avoid extmod include (must be #define for preprocessor check)
#define mp_hal_pin_obj_t int
#define mp_hal_get_pin_obj(pin) (0)
#define mp_hal_pin_read(pin) (0)
#define mp_hal_pin_write(pin, v) ((void)(v))

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_stdout_tx_str(const char *str);
int mp_hal_stdin_rx_chr(void);
void mp_hal_delay_ms(mp_uint_t ms);

static inline void mp_hal_set_interrupt_char(int c) { (void)c; }

#endif
