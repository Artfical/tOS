#include "usb_audio.h"
#include "io.h"
#include "klog.h"

int usb_audio_init(void)
{
    klog_write("usb_audio: stub init\n");
    return 0;
}

void usb_audio_shutdown(void)
{
    klog_write("usb_audio: stub shutdown\n");
}
