/* No real math.h exists elsewhere in this freestanding kernel to wrap
 * (only kernel/micropython/ports/tos/math_stubs.c's float functions,
 * used ad hoc without a shared header -- kernel/doom/port/math.h
 * already takes this same self-contained approach for DOOM). Not
 * chained via #include_next to a system header: in C++ mode, g++'s
 * own <math.h> pulls in the full hosted libstdc++ <cmath> (including
 * templated special functions that don't compile freestanding at
 * all), which is exactly what this file exists to avoid. Declares
 * only the double-precision functions kernel/wolf3d's source actually
 * calls (sin/atan2/tan/sqrt/atan -- see wolf_compat.cpp for the
 * implementations, delegating to math_stubs.c's already-tested float
 * versions the same way doom_compat.c does for DOOM's two). */
#ifndef WOLF3D_MATH_H
#define WOLF3D_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double pow(double x, double y);

#ifdef __cplusplus
}
#endif

#endif
