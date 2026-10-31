#include "common.h"
#include <ffi.h>
static int add3(int a, int b, int c) { return a + b + c; }
static double mix(double x, int n, float f) { return x * n + f; }
typedef struct { int a; double b; char c; } S;
static S make_s(int a, double b) { S s = { a, b, 'z' }; return s; }
static long sum_many(long a, long b, long c, long d, long e, long f, long g, long h) { return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h; }
static void closure_fn(ffi_cif *cif, void *ret, void **args, void *user)
{
    (void)cif;
    *(int *)ret = *(int *)args[0] * *(int *)user + *(int *)args[1];
}
#define STEP(m) do { printf("  ffi step: %s\n", m); fflush(stdout); } while (0)
int main(void)
{
    STEP("start");
    ffi_cif cif;
    ffi_type *t3[3] = { &ffi_type_sint, &ffi_type_sint, &ffi_type_sint };
    CHECK(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_sint, t3) == FFI_OK, "prep_cif add3");
    int a = 10, b = 20, c = 12; void *av[3] = { &a, &b, &c }; ffi_arg rv = 0;
    STEP("ffi_call");
    ffi_call(&cif, FFI_FN(add3), &rv, av);
    CHECK((int)rv == 42, "ffi_call int(int,int,int)");
    ffi_type *tm[3] = { &ffi_type_double, &ffi_type_sint, &ffi_type_float };
    ffi_cif c2; ffi_prep_cif(&c2, FFI_DEFAULT_ABI, 3, &ffi_type_double, tm);
    double x = 2.5; int n = 4; float f = 0.25f; void *av2[3] = { &x, &n, &f }; double dr = 0;
    STEP("ffi_call");
    ffi_call(&c2, FFI_FN(mix), &dr, av2);
    CHECK(dr == 10.25, "ffi_call double(double,int,float)");
    ffi_type *elems[4] = { &ffi_type_sint, &ffi_type_double, &ffi_type_schar, NULL };
    ffi_type st = { 0, 0, FFI_TYPE_STRUCT, elems };
    ffi_type *ts[2] = { &ffi_type_sint, &ffi_type_double };
    ffi_cif c3; CHECK(ffi_prep_cif(&c3, FFI_DEFAULT_ABI, 2, &st, ts) == FFI_OK, "prep_cif struct return");
    int sa = 7; double sb = 1.5; void *av3[2] = { &sa, &sb }; S sr;
    STEP("ffi_call");
    ffi_call(&c3, FFI_FN(make_s), &sr, av3);
    CHECK(sr.a == 7 && sr.b == 1.5 && sr.c == 'z', "a struct comes back");
    ffi_type *t8[8]; for (int i = 0; i < 8; i++) t8[i] = &ffi_type_slong;
    ffi_cif c4; ffi_prep_cif(&c4, FFI_DEFAULT_ABI, 8, &ffi_type_slong, t8);
    long v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }; void *av4[8]; for (int i = 0; i < 8; i++) av4[i] = &v[i];
    long lr = 0; STEP("ffi_call");
    ffi_call(&c4, FFI_FN(sum_many), &lr, av4);
    CHECK(lr == 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64, "eight arguments (some on the stack)");
    /* a closure: a C function pointer that calls back into closure_fn */
    STEP("void *code; ffi_closure *cl = ffi_closure_alloc");
    void *code; ffi_closure *cl = ffi_closure_alloc(sizeof(ffi_closure), &code);
    CHECK(cl != NULL, "ffi_closure_alloc");
    if (cl) {
        ffi_type *t2[2] = { &ffi_type_sint, &ffi_type_sint };
        ffi_cif cc; ffi_prep_cif(&cc, FFI_DEFAULT_ABI, 2, &ffi_type_sint, t2);
        int mult = 100;
        CHECK(ffi_prep_closure_loc(cl, &cc, closure_fn, &mult, code) == FFI_OK, "ffi_prep_closure_loc");
        int (*fp)(int, int) = (int (*)(int, int))code;
        STEP("call closure");
        CHECK(fp(3, 4) == 304 && fp(5, 6) == 506, "a closure is called like a function");
        ffi_closure_free(cl);
    }
    DONE("libffi");
}
