#include "virtio_scsi.h"
#include "io.h"
#include "klog.h"

int virtio_scsi_init(void)
{
    klog_write("virtio_scsi: stub init\n");
    return 0;
}

void virtio_scsi_shutdown(void)
{
    klog_write("virtio_scsi: stub shutdown\n");
}
