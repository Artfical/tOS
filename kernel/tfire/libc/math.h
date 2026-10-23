#ifndef TFIRE_MATH_H
#define TFIRE_MATH_H
/* <math.h> for tFire's JavaScript engine: musl's libm (third_party/musl-libm), built for software floating
 * point (compiler-rt) so the engine never touches the FPU */
typedef double double_t;
typedef float float_t;
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define FP_ILOGBNAN (-1-0x7fffffff)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double sqrt(double x);
double cbrt(double x);
double pow(double x, double y);
double exp(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double hypot(double x, double y);
double ldexp(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
double fmin(double a, double b);
double fmax(double a, double b);
double copysign(double x, double y);
double nearbyint(double x);
double rint(double x);
/* the float versions, by way of the double ones */
static inline float sqrtf(float x) { return (float)sqrt(x); }
static inline float fabsf(float x) { return x < 0 ? -x : x; }
static inline float floorf(float x) { return (float)floor(x); }
static inline float ceilf(float x) { return (float)ceil(x); }
static inline float roundf(float x) { return (float)round(x); }
static inline float rintf(float x) { return (float)rint(x); }
static inline float nearbyintf(float x) { return (float)nearbyint(x); }
static inline float truncf(float x) { return (float)trunc(x); }
static inline float copysignf(float x, float y) { return (float)copysign(x, y); }
static inline float fmodf(float x, float y) { return (float)fmod(x, y); }
static inline float sinf(float x) { return (float)sin(x); }
static inline float cosf(float x) { return (float)cos(x); }
static inline float tanf(float x) { return (float)tan(x); }
static inline float acosf(float x) { return (float)acos(x); }
static inline float asinf(float x) { return (float)asin(x); }
static inline float atanf(float x) { return (float)atan(x); }
static inline float atan2f(float y, float x) { return (float)atan2(y, x); }
static inline float powf(float x, float y) { return (float)pow(x, y); }
static inline float expf(float x) { return (float)exp(x); }
static inline float logf(float x) { return (float)log(x); }
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }
#endif
