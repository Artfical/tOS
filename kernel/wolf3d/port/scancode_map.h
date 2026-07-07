#ifndef WOLF3D_SCANCODE_MAP_H
#define WOLF3D_SCANCODE_MAP_H
/* Drop-in replacement for the two std::unordered_map<ScanCode, T>
 * uses in kernel/wolf3d/id_in.h/wl_menu.cpp -- tOS has no libstdc++,
 * so no real STL containers. Every actual key used across this
 * engine is either a small raw ASCII-ish value or SDL.h's
 * SDLK_SCANCODE_MASK bit OR'd with a small (<256) USB-HID-derived
 * scancode (see SDL.h's SDLK_* constants), so a sparse hash table
 * isn't needed at all -- two flat 256-entry arrays, chosen by
 * whether that mask bit is set, cover every key this engine ever
 * looks up or stores, at a fraction of the code. */
#include "SDL.h"

template<typename T>
class ScanCodeMap {
public:
    ScanCodeMap()
    {
        for (int i = 0; i < 256; i++) {
            ascii_[i] = T();
            scan_[i] = T();
            has_ascii_[i] = false;
            has_scan_[i] = false;
        }
    }

    T &operator[](int key)
    {
        if (key & SDLK_SCANCODE_MASK) {
            int i = key & 0xFF;
            has_scan_[i] = true;
            return scan_[i];
        }
        int i = key & 0xFF;
        has_ascii_[i] = true;
        return ascii_[i];
    }

    bool contains(int key) const
    {
        if (key & SDLK_SCANCODE_MASK) return has_scan_[key & 0xFF];
        return has_ascii_[key & 0xFF];
    }

    /* id_in.cpp's IN_ClearKeysDown() resets every key's state back to
     * "not pressed" (unordered_map's clear() would just empty it, but
     * this engine reads Keyboard[key] unconditionally elsewhere -- a
     * key that's `contains()`-false but has a stale T() default would
     * behave identically for the boolean-value uses this reset is
     * actually for). */
    void clear()
    {
        for (int i = 0; i < 256; i++) {
            ascii_[i] = T();
            scan_[i] = T();
        }
    }

private:
    T ascii_[256];
    T scan_[256];
    bool has_ascii_[256];
    bool has_scan_[256];
};

#endif
