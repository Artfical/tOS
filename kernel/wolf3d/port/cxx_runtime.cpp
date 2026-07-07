/* Minimal freestanding C++ runtime support for the vendored Wolf4SDL
 * engine (kernel/wolf3d/) -- not part of Wolf4SDL/id Software's
 * source. tOS has no libstdc++, no exception unwinding, and no
 * threads, so WOLF_CXXFLAGS disables exceptions/RTTI/thread-safe
 * statics entirely; only the handful of ABI hooks the compiler still
 * unconditionally emits calls to (operator new/delete, and the vtable
 * stub for a pure virtual call) need to exist. */
#include <stddef.h>

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

void *operator new(size_t size) { return malloc(size); }
void *operator new[](size_t size) { return malloc(size); }
void operator delete(void *ptr) { free(ptr); }
void operator delete[](void *ptr) { free(ptr); }
void operator delete(void *ptr, size_t) { free(ptr); }
void operator delete[](void *ptr, size_t) { free(ptr); }

extern "C" void __cxa_pure_virtual(void)
{
    /* A pure-virtual call with nothing sensible to do about it at
     * runtime (no exceptions, no stderr in the usual sense) -- spin
     * rather than silently returning into undefined behavior, same
     * as kernel/lib/stdlib.c's abort()/exit(). */
    for (;;) { }
}
