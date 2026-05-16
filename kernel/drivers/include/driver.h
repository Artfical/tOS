#ifndef DRIVER_H
#define DRIVER_H
#include <stdint.h>
#define DRIVER_STATUS_NOT_FOUND -1
#define DRIVER_STATUS_OK 0
#define DRIVER_STATUS_ERROR 1
#define DRIVER_STATUS_UNSUPPORTED 2
typedef struct {
    const char *name;
    int (*init)(void);
    int initialized;
} driver_t;
typedef enum {
    DRIVER_CLASS_BUS,
    DRIVER_CLASS_STORAGE,
    DRIVER_CLASS_NETWORK,
    DRIVER_CLASS_USB,
    DRIVER_CLASS_AUDIO,
    DRIVER_CLASS_VIDEO,
    DRIVER_CLASS_INPUT,
    DRIVER_CLASS_SYSTEM,
    DRIVER_CLASS_MISC
} driver_class_t;
#endif
