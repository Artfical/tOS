#include "accelerometer.h"
#include "io.h"
#include "klog.h"

int accelerometer_init(void)
{
    klog_write("accelerometer: stub init\n");
    return 0;
}

void accelerometer_shutdown(void)
{
    klog_write("accelerometer: stub shutdown\n");
}
