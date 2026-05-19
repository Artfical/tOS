#include "virtio_gpu.h"
#include "io.h"
#include "klog.h"

int virtio_gpu_init(void)
{
    klog_write("virtio_gpu: stub init\n");
    return 0;
}

void virtio_gpu_shutdown(void)
{
    klog_write("virtio_gpu: stub shutdown\n");
}
