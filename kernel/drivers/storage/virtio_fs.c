#include "virtio_fs.h"
#include "io.h"
#include "klog.h"

int virtio_fs_init(void)
{
    klog_write("virtio_fs: stub init\n");
    return 0;
}

void virtio_fs_shutdown(void)
{
    klog_write("virtio_fs: stub shutdown\n");
}
