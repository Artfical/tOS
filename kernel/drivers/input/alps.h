#ifndef TOS_ALPS_H
#define TOS_ALPS_H

#include <stdint.h>

int alps_detect(void);
int alps_init(void);
void alps_shutdown(void);

#endif
