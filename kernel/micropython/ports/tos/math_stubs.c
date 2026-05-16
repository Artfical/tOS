// Minimal math stubs for MicroPython float support
// (freestanding mode has no libm)

#include <stddef.h>

void __assert_fail(const char *expr, const char *file, int line) {
    for (;;);
}

float fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    int q = (int)(x / y);
    return x - q * y;
}

float copysignf(float x, float y) {
    unsigned int ix = *(unsigned int *)&x;
    unsigned int iy = *(unsigned int *)&y;
    ix = (ix & 0x7FFFFFFF) | (iy & 0x80000000);
    return *(float *)&ix;
}

float floorf(float x) {
    int i = (int)x;
    float f = (float)i;
    if (x < 0.0f && f != x) f -= 1.0f;
    return f;
}

float powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (y == 1.0f) return x;
    int exp = (int)y;
    if ((float)exp == y && exp > 0) {
        float r = 1.0f;
        for (int i = 0; i < exp; i++) r *= x;
        return r;
    }
    return x;
}

float nanf(const char *tagp) {
    (void)tagp;
    unsigned int nan = 0x7FC00000;
    return *(float *)&nan;
}

float ceilf(float x) {
    int i = (int)x;
    float f = (float)i;
    if (x > 0.0f && f != x) f += 1.0f;
    return f;
}

float sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x;
    for (int i = 0; i < 10; i++) {
        r = (r + x / r) * 0.5f;
    }
    return r;
}

float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

float atan2f(float y, float x) {
    if (x == 0.0f) {
        if (y > 0.0f) return 1.570796f;
        if (y < 0.0f) return -1.570796f;
        return 0.0f;
    }
    float a = y / x;
    float a2 = a * a;
    float at = a * (1.0f - a2 / 3.0f + a2 * a2 / 5.0f);
    if (x < 0.0f) at += (y >= 0.0f ? 3.141593f : -3.141593f);
    return at;
}

float sinf(float x) {
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    return x - x3 / 6.0f + x5 / 120.0f;
}

float cosf(float x) {
    return sinf(x + 1.570796f);
}

float ldexpf(float x, int exp) {
    if (exp > 0) {
        for (int i = 0; i < exp; i++) x *= 2.0f;
    } else {
        for (int i = 0; i < -exp; i++) x *= 0.5f;
    }
    return x;
}

float truncf(float x) {
    return (float)(int)x;
}

float modff(float x, float *iptr) {
    int i = (int)x;
    *iptr = (float)i;
    return x - *iptr;
}

float frexpf(float x, int *exp) {
    if (x == 0.0f) { *exp = 0; return 0.0f; }
    unsigned int *ix = (unsigned int *)&x;
    int e = ((*ix >> 23) & 0xFF) - 127;
    *exp = e + 1;
    *ix &= 0x807FFFFF;
    *ix |= 0x3F000000;
    return x;
}

float atanf(float x) {
    float x2 = x * x;
    return x * (1.0f - x2 / 3.0f + x2 * x2 / 5.0f - x2 * x2 * x2 / 7.0f);
}

float asinf(float x) {
    return atan2f(x, sqrtf(1.0f - x * x));
}

float acosf(float x) {
    return 1.570796f - asinf(x);
}

float tanf(float x) {
    return sinf(x) / cosf(x);
}

float expf(float x) {
    float r = 1.0f;
    float t = 1.0f;
    for (int i = 1; i < 20; i++) {
        t *= x / i;
        r += t;
    }
    return r;
}

float logf(float x) {
    if (x <= 0.0f) return -1.0f / 0.0f;
    float r = 0.0f;
    float t = (x - 1.0f) / (x + 1.0f);
    float t2 = t * t;
    float p = t;
    for (int i = 1; i < 20; i += 2) {
        r += p / i;
        p *= t2;
    }
    return 2.0f * r;
}

float nearbyintf(float x) {
    float f = floorf(x);
    if (x - f >= 0.5f) return f + 1.0f;
    return f;
}
