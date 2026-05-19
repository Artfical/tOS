#include "dm_crypt.h"
#include "io.h"
#include "klog.h"

int dm_crypt_init(void)
{
    klog_write("dm_crypt: stub init\n");
    return 0;
}

void dm_crypt_shutdown(void)
{
    klog_write("dm_crypt: stub shutdown\n");
}
