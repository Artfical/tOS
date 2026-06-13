#include "debugmon.h"
#include "serial.h"
#include "string.h"

#define DEBUGMON_MAGIC0 0xDE
#define DEBUGMON_MAGIC1 0xAD

#define PKT_LOG     0x01
#define PKT_MEMDUMP 0x02
#define PKT_REGDUMP 0x03
#define PKT_IRQ     0x04
#define PKT_PANIC   0x05

/* Fixed order expected by tOS_monitor's packet_parser.py REGISTER_ORDER. */
static const char *const reg_names[13] = {
    "EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "ESP", "EBP",
    "EIP", "EFLAGS", "CR0", "CR2", "CR3",
};

static inline uint32_t read_cr0(void)
{
    uint32_t v;
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline uint32_t read_cr2(void)
{
    uint32_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint32_t read_cr3(void)
{
    uint32_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void write_u32_le(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void debugmon_send_packet(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t checksum = 0;
    for (uint16_t i = 0; i < len; i++)
        checksum ^= payload[i];

    serial_putchar((char)DEBUGMON_MAGIC0);
    serial_putchar((char)DEBUGMON_MAGIC1);
    serial_putchar((char)type);
    serial_putchar((char)(len & 0xFF));
    serial_putchar((char)((len >> 8) & 0xFF));
    for (uint16_t i = 0; i < len; i++)
        serial_putchar((char)payload[i]);
    serial_putchar((char)checksum);
}

void debugmon_init(void)
{
    debugmon_send_log("[INFO] [DEBUGMON] tOS debug protocol online");
}

void debugmon_send_log(const char *msg)
{
    uint8_t payload[200];
    uint16_t len = (uint16_t)strlen(msg);

    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        len--;

    if (len > sizeof(payload) - 1)
        len = sizeof(payload) - 1;

    for (uint16_t i = 0; i < len; i++)
        payload[i] = (uint8_t)msg[i];
    payload[len] = 0;

    debugmon_send_packet(PKT_LOG, payload, (uint16_t)(len + 1));
}

void debugmon_send_irq(uint8_t irq_number, uint32_t handler_addr)
{
    uint8_t payload[5];
    payload[0] = irq_number;
    write_u32_le(&payload[1], handler_addr);
    debugmon_send_packet(PKT_IRQ, payload, sizeof(payload));
}

void debugmon_send_regdump(registers_t *regs)
{
    uint32_t values[13];
    values[0] = regs->eax;
    values[1] = regs->ebx;
    values[2] = regs->ecx;
    values[3] = regs->edx;
    values[4] = regs->esi;
    values[5] = regs->edi;
    values[6] = regs->esp;
    values[7] = regs->ebp;
    values[8] = regs->eip;
    values[9] = regs->eflags;
    values[10] = read_cr0();
    values[11] = read_cr2();
    values[12] = read_cr3();

    uint8_t payload[13 * 8];
    for (int i = 0; i < 13; i++) {
        const char *name = reg_names[i];
        for (int j = 0; j < 4; j++)
            payload[i * 8 + j] = (uint8_t)name[j];
        write_u32_le(&payload[i * 8 + 4], values[i]);
    }

    debugmon_send_packet(PKT_REGDUMP, payload, sizeof(payload));
}

void debugmon_send_panic(uint32_t error_code, uint32_t eip, const char *msg)
{
    uint8_t payload[8 + 128];
    uint16_t msg_len = (uint16_t)strlen(msg);

    if (msg_len > sizeof(payload) - 8)
        msg_len = sizeof(payload) - 8;

    write_u32_le(&payload[0], error_code);
    write_u32_le(&payload[4], eip);
    for (uint16_t i = 0; i < msg_len; i++)
        payload[8 + i] = (uint8_t)msg[i];

    debugmon_send_packet(PKT_PANIC, payload, (uint16_t)(8 + msg_len));
}

void debugmon_send_memdump(uint32_t address, const uint8_t *data, uint16_t len)
{
    uint8_t payload[4 + 256];

    if (len > sizeof(payload) - 4)
        len = sizeof(payload) - 4;

    write_u32_le(&payload[0], address);
    for (uint16_t i = 0; i < len; i++)
        payload[4 + i] = data[i];

    debugmon_send_packet(PKT_MEMDUMP, payload, (uint16_t)(4 + len));
}

int debugmon_poll_regdump_request(void)
{
    int requested = 0;
    while (serial_received()) {
        if (serial_getchar() == DEBUGMON_REGDUMP_REQUEST)
            requested = 1;
    }
    return requested;
}
