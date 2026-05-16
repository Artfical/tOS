#ifndef MICROPY_INCLUDED_TOS_MPHALPORT_H
#define MICROPY_INCLUDED_TOS_MPHALPORT_H

#include "py/mpconfig.h"
#include <stddef.h>

#define mp_hal_pin_obj_t int
#define mp_hal_get_pin_obj(pin) (0)
#define mp_hal_pin_read(pin) (0)
#define mp_hal_pin_write(pin, v) ((void)(v))

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_stdout_tx_str(const char *str);
int mp_hal_stdin_rx_chr(void);
void mp_hal_delay_ms(mp_uint_t ms);

extern volatile int interrupt_char;
extern void (*interrupt_callback)(void);

static inline void mp_hal_set_interrupt_char(int c) {
    interrupt_char = c;
    if (c == -1) interrupt_callback = NULL;
}

// Custom cursor movement without VT100 escapes
#define MICROPY_HAL_HAS_VT100 (0)

static inline void mp_hal_move_cursor_back(unsigned int pos) {
    while (pos--) mp_hal_stdout_tx_strn("\b", 1);
}

static inline void mp_hal_erase_line_from_cursor(unsigned int n_chars_to_erase) {
    for (unsigned int i = 0; i < n_chars_to_erase; i++)
        mp_hal_stdout_tx_strn(" ", 1);
    for (unsigned int i = 0; i < n_chars_to_erase; i++)
        mp_hal_stdout_tx_strn("\b", 1);
}

#endif
