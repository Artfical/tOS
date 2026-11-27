/* tOS has no D-Bus and no screen reader: GTK asks the accessibility bridge to start, and this one says it is not there. */
#include <stddef.h>
int atk_bridge_adaptor_init(int *argc, char **argv[]) { (void)argc; (void)argv; return 0; }
void atk_bridge_adaptor_cleanup(void) { }
