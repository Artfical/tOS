#include "nvme.h"
#include "pci.h"
#include "memory.h"
#include "string.h"
#include "paging.h"
#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30
#define NVME_REG_DBL   0x1000

#define ADMIN_QUEUE_DEPTH 4

static inline uint32_t reg_r32(nvme_device_t *dev, uint32_t off)
{
    return *(volatile uint32_t *)(dev->regs + off);
}

static inline void reg_w32(nvme_device_t *dev, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(dev->regs + off) = val;
}

static inline void reg_w64(nvme_device_t *dev, uint32_t off, uint64_t val)
{
    reg_w32(dev, off, (uint32_t)(val & 0xFFFFFFFF));
    reg_w32(dev, off + 4, (uint32_t)(val >> 32));
}

static void ring_sq_doorbell(nvme_device_t *dev, int qid, uint16_t tail)
{
    uint32_t off = NVME_REG_DBL + (2 * qid) * dev->doorbell_stride;
    reg_w32(dev, off, tail);
}

static void ring_cq_doorbell(nvme_device_t *dev, int qid, uint16_t head)
{
    uint32_t off = NVME_REG_DBL + (2 * qid + 1) * dev->doorbell_stride;
    reg_w32(dev, off, head);
}

static int nvme_admin_cmd(nvme_device_t *dev, nvme_command_t *cmd, nvme_completion_t *out_cpl)
{
    uint16_t cid = dev->next_cmd_id++;
    cmd->dword0 = (cmd->dword0 & 0x0000FFFF) | ((uint32_t)cid << 16);

    uint16_t tail = dev->asq_tail;
    dev->asq[tail] = *cmd;
    tail = (tail + 1) % ADMIN_QUEUE_DEPTH;
    dev->asq_tail = tail;
    ring_sq_doorbell(dev, 0, tail);

    int t = 5000000;
    volatile nvme_completion_t *cpl = &dev->acq[dev->acq_head];
    while (--t) {
        uint16_t status = cpl->status;
        if ((status & 1) == dev->acq_phase) break;
    }
    if (!t) return -1;

    if (out_cpl) *out_cpl = *cpl;
    int ok = ((cpl->status >> 1) & 0x7FF) == 0;

    dev->acq_head = (dev->acq_head + 1) % ADMIN_QUEUE_DEPTH;
    if (dev->acq_head == 0) dev->acq_phase ^= 1;
    ring_cq_doorbell(dev, 0, dev->acq_head);

    return ok ? 0 : -1;
}

static int nvme_io_cmd(nvme_device_t *dev, nvme_command_t *cmd)
{
    uint16_t cid = dev->next_cmd_id++;
    cmd->dword0 = (cmd->dword0 & 0x0000FFFF) | ((uint32_t)cid << 16);

    uint16_t tail = dev->iosq_tail;
    dev->iosq[tail] = *cmd;
    tail = (tail + 1) % NVME_MAX_QUEUE_ENTRIES;
    dev->iosq_tail = tail;
    ring_sq_doorbell(dev, 1, tail);

    int t = 5000000;
    volatile nvme_completion_t *cpl = &dev->iocq[dev->iocq_head];
    while (--t) {
        uint16_t status = cpl->status;
        if ((status & 1) == dev->iocq_phase) break;
    }
    if (!t) return -1;

    int ok = ((cpl->status >> 1) & 0x7FF) == 0;

    dev->iocq_head = (dev->iocq_head + 1) % NVME_MAX_QUEUE_ENTRIES;
    if (dev->iocq_head == 0) dev->iocq_phase ^= 1;
    ring_cq_doorbell(dev, 1, dev->iocq_head);

    return ok ? 0 : -1;
}

static int nvme_identify_namespace(nvme_device_t *dev)
{
    uint8_t *buf = (uint8_t *)alloc_physical_page();
    if (!buf) return -1;
    memset(buf, 0, 4096);

    nvme_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = NVME_ADMIN_IDENTIFY;
    cmd.dword1 = 1;
    cmd.dword6 = (uint32_t)buf;
    cmd.dword10 = 0;

    if (nvme_admin_cmd(dev, &cmd, NULL) != 0) return -1;

    uint64_t nsze;
    memcpy(&nsze, buf + 0, 8);
    uint8_t flbas = buf[26] & 0x0F;
    uint8_t lbads = buf[128 + flbas * 4 + 2];

    dev->total_sectors = nsze;
    dev->sector_size = lbads ? (1u << lbads) : 512;
    return 0;
}

static int nvme_create_io_queues(nvme_device_t *dev)
{
    dev->iocq = (volatile nvme_completion_t *)alloc_physical_page();
    dev->iosq = (nvme_command_t *)alloc_physical_page();
    dev->prp_list = (uint8_t *)alloc_physical_page();
    if (!dev->iocq || !dev->iosq || !dev->prp_list) return -1;
    memset((void *)dev->iocq, 0, 4096);
    memset(dev->iosq, 0, 4096);
    dev->iocq_head = 0;
    dev->iocq_phase = 1;
    dev->iosq_tail = 0;

    nvme_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = NVME_ADMIN_CREATE_CQ;
    cmd.dword6 = (uint32_t)dev->iocq;
    cmd.dword10 = (uint32_t)((NVME_MAX_QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.dword11 = 1;
    if (nvme_admin_cmd(dev, &cmd, NULL) != 0) return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = NVME_ADMIN_CREATE_SQ;
    cmd.dword6 = (uint32_t)dev->iosq;
    cmd.dword10 = (uint32_t)((NVME_MAX_QUEUE_ENTRIES - 1) << 16) | 1;
    cmd.dword11 = (uint32_t)(1 << 16) | 1;
    if (nvme_admin_cmd(dev, &cmd, NULL) != 0) return -1;

    return 0;
}

int nvme_init(nvme_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));

    pci_device_t pci_devs[4];
    int n = pci_find_devices(0x01, 0x08, pci_devs, 4);
    if (!n) return -1;

    pci_device_t pd = pci_devs[0];
    uint32_t bar0 = pci_get_bar(pd.bus, pd.device, pd.func, 0);
    uint32_t bar1 = pci_get_bar(pd.bus, pd.device, pd.func, 1);
    int is64 = ((bar0 >> 1) & 0x3) == 0x2;
    if (is64 && bar1 != 0) return -1;

    dev->bar0 = bar0 & 0xFFFFFFF0;
    dev->regs = (volatile uint8_t *)dev->bar0;

    uint32_t cmd = pci_read_config(pd.bus, pd.device, pd.func, 4) & 0xFFFF;
    pci_write_config(pd.bus, pd.device, pd.func, 4, cmd | 0x06 | (1u << 10));

    paging_map_range(dev->bar0 & ~0xFFFu, dev->bar0 & ~0xFFFu, 0x4000, PTE_PRESENT | PTE_WRITABLE);

    uint32_t cap_hi = reg_r32(dev, NVME_REG_CAP + 4);
    dev->doorbell_stride = 4u << (cap_hi & 0xF);

    reg_w32(dev, NVME_REG_CC, 0);
    int t = 2000000;
    while ((reg_r32(dev, NVME_REG_CSTS) & 1) && --t);

    dev->asq = (nvme_command_t *)alloc_physical_page();
    dev->acq = (volatile nvme_completion_t *)alloc_physical_page();
    if (!dev->asq || !dev->acq) return -1;
    memset(dev->asq, 0, 4096);
    memset((void *)dev->acq, 0, 4096);
    dev->asq_tail = 0;
    dev->acq_head = 0;
    dev->acq_phase = 1;
    dev->next_cmd_id = 1;

    reg_w32(dev, NVME_REG_AQA, ((ADMIN_QUEUE_DEPTH - 1) << 16) | (ADMIN_QUEUE_DEPTH - 1));
    reg_w64(dev, NVME_REG_ASQ, (uint32_t)dev->asq);
    reg_w64(dev, NVME_REG_ACQ, (uint32_t)dev->acq);

    reg_w32(dev, NVME_REG_CC, (4u << 20) | (6u << 16) | 1u);

    t = 2000000;
    while (!(reg_r32(dev, NVME_REG_CSTS) & 1) && --t);
    if (!t) return -1;

    if (nvme_identify_namespace(dev) != 0) return -1;
    if (nvme_create_io_queues(dev) != 0) return -1;

    dev->present = 1;
    return 0;
}

static void nvme_build_prp(nvme_device_t *dev, uint32_t phys, uint32_t nbytes, uint32_t *prp1, uint32_t *prp2)
{
    *prp1 = phys;
    uint32_t first_page_rem = 4096 - (phys & 0xFFF);
    if (nbytes <= first_page_rem) {
        *prp2 = 0;
        return;
    }
    uint32_t remaining = nbytes - first_page_rem;
    uint32_t next_page = (phys & ~0xFFFu) + 4096;
    if (remaining <= 4096) {
        *prp2 = next_page;
        return;
    }
    uint32_t *list = (uint32_t *)dev->prp_list;
    uint32_t page = next_page;
    int idx = 0;
    while (remaining > 0) {
        list[idx++] = page;
        page += 4096;
        remaining = (remaining > 4096) ? remaining - 4096 : 0;
    }
    *prp2 = (uint32_t)dev->prp_list;
}

static int nvme_io(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buf, uint8_t opcode)
{
    if (!dev->present) return -1;
    uint32_t nbytes = count * dev->sector_size;
    uint32_t prp1, prp2;
    nvme_build_prp(dev, (uint32_t)buf, nbytes, &prp1, &prp2);

    nvme_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = opcode;
    cmd.dword1 = 1;
    cmd.dword6 = prp1;
    cmd.dword8 = prp2;
    cmd.dword10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.dword11 = (uint32_t)(lba >> 32);
    cmd.dword12 = (count - 1) & 0xFFFF;

    return nvme_io_cmd(dev, &cmd);
}

int nvme_read(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    return nvme_io(dev, lba, count, buf, NVME_CMD_READ);
}

int nvme_write(nvme_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return nvme_io(dev, lba, count, (void *)buf, NVME_CMD_WRITE);
}
