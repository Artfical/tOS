#include "ps2.h"
#include "io.h"
static int ps2_wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & 2)) return 0;
    return -1;
}
static int ps2_wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 1) return 0;
    return -1;
}
int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_write()) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}
int ps2_send_data(uint8_t data)
{
    if (ps2_wait_write()) return -1;
    outb(PS2_DATA, data);
    return 0;
}
uint8_t ps2_read_data(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}
int ps2_self_test(void)
{
    if (ps2_send_command(PS2_CMD_TEST_CTRL)) return -1;
    return ps2_read_data() == 0x55 ? 0 : -1;
}
int ps2_init(ps2_controller_t *ctrl)
{
    ctrl->present = 0;
    ctrl->dual_channel = 0;
    ps2_send_command(PS2_CMD_DISABLE_PORT1);
    ps2_send_command(PS2_CMD_DISABLE_PORT2);
    ps2_read_data();
    if (ps2_self_test()) return -1;
    ps2_send_command(PS2_CMD_ENABLE_PORT1);
    ctrl->present = 1;
    return 0;
}
