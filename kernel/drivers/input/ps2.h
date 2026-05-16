#ifndef PS2_H
#define PS2_H
#include <stdint.h>
#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64
#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_DISABLE_PORT1 0xAD
#define PS2_CMD_ENABLE_PORT1 0xAE
#define PS2_CMD_DISABLE_PORT2 0xA7
#define PS2_CMD_ENABLE_PORT2 0xA8
#define PS2_CMD_TEST_PORT1 0xAB
#define PS2_CMD_TEST_PORT2 0xA9
#define PS2_CMD_TEST_CTRL 0xAA
#define PS2_CFG_PORT1_INT 0x01
#define PS2_CFG_PORT2_INT 0x02
#define PS2_CFG_PORT1_CLOCK 0x10
#define PS2_CFG_PORT2_CLOCK 0x20
#define PS2_CFG_PORT1_TRANS 0x40
typedef struct {
    int present;
    int dual_channel;
} ps2_controller_t;
int ps2_init(ps2_controller_t *ctrl);
int ps2_send_command(uint8_t cmd);
int ps2_send_data(uint8_t data);
uint8_t ps2_read_data(void);
int ps2_self_test(void);
#endif
