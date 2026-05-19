#include "virtio_rng.h"
#include "io.h"
#include "klog.h"

int virtio_rng_init(void)
{
    klog_write("virtio_rng: stub init\n");
    return 0;
}

void virtio_rng_shutdown(void)
{
    klog_write("virtio_rng: stub shutdown\n");
}
