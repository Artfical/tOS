#ifndef TFIRE_FENV_H
#define TFIRE_FENV_H
/* floating point here is done in software (compiler-rt): there are no exception flags to test */
#define FE_INEXACT 0
#define FE_ALL_EXCEPT 0
#define FE_TONEAREST 0
static inline int fetestexcept(int e) { (void)e; return 0; }
static inline int feclearexcept(int e) { (void)e; return 0; }
#endif
