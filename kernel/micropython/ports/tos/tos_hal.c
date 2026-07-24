#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/misc.h"
#include "serial.h"
#include "terminal.h"
#include "keyboard.h"
#include <string.h>

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    /* terminal_putchar() already mirrors every char to serial_putchar()
     * internally, so an extra explicit serial_putchar() call here
     * doubled each character on output. */
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') terminal_putchar('\r');
        terminal_putchar(str[i]);
    }
    return len;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    mp_hal_stdout_tx_strn(str, len);
}

void mp_hal_stdout_tx_str(const char *str) {
    mp_hal_stdout_tx_strn(str, strlen(str));
}

int mp_hal_stdin_rx_chr(void) {
    return (unsigned char)keyboard_getchar();
}

void mp_hal_delay_ms(mp_uint_t ms) {
    volatile int count = (int)(ms * 1000);
    while (count--) {
        for (volatile int i = 0; i < 100; i++);
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    volatile int count = (int)us;
    while (count--) {
        for (volatile int i = 0; i < 10; i++);
    }
}
