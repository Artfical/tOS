/*
 * WebAssembly for tFire: wasm3 (third_party/wasm3, an interpreter) under the natives prelude.js builds the
 * WebAssembly object from:
 *
 *   __tf.wasmCompile(bytes)            -> { mod, imports: [{module, name, kind, type}], exports: [{name, kind, type}] }
 *   __tf.wasmInstantiate(mod, values)  -> inst      (values: one per import, in the module's order)
 *   __tf.wasmCall(inst, name, args)    -> the result (undefined, a number, a BigInt, or an array of them)
 *   __tf.wasmMemory(inst)              -> the memory as an ArrayBuffer (a new one after it grew; the old is detached)
 *   __tf.wasmGrow(inst, pages)         -> the old size in pages, or -1
 *   __tf.wasmGlobal(inst, name[, v])   -> an exported global's value (and sets it when v is given)
 *
 * kind: 0 function, 1 table, 2 memory, 3 global. type: a function's "params:results" in wasm3's letters (i I f F),
 * a global's letter. Errors are thrown as Error objects with wasmKind = "compile", "link" or "runtime", which the
 * prelude turns into WebAssembly.CompileError and the rest.
 *
 * Each instance is a wasm3 environment and runtime of its own, with its own copy of the module's bytes (wasm3 reads
 * them in place). An import is a JS function behind a trampoline; when it calls back into the instance, the call
 * runs on the stack above the import's own frame (wasm3 starts every call at the bottom of the stack).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "wasm3.h"
#include "m3_env.h"
#include "m3_function.h"

#define WASM_STACK (512 * 1024)
#define MAX_IMPORTS 512

typedef struct {
    uint8_t *bytes;
    uint32_t len;
} wmod;

struct winst;

typedef struct {
    struct winst *in;
    JSValue fn;
    char sig[64];                  /* "params:results" */
} wthunk;

typedef struct winst {
    JSContext *ctx;
    IM3Environment env;
    IM3Runtime rt;
    IM3Module mod;
    uint8_t *bytes;
    wthunk *thunks;
    int nthunks;
    JSValue pending;               /* an exception a JS import threw, to throw again from the call into wasm */
    int has_pending;
    JSValue buffer;                /* the memory as an ArrayBuffer, while it stays where it is */
    uint8_t *buf_at;
    uint32_t buf_len;
} winst;

static JSClassID mod_class, inst_class;

/* ------------------------------------------------------------------ the module's sections, read by hand */

typedef struct { const uint8_t *p, *e; int bad; } rd;

static uint32_t leb(rd *r)
{
    uint32_t v = 0;
    int sh = 0;
    while (r->p < r->e) {
        uint8_t b = *r->p++;
        if (sh < 32) v |= (uint32_t)(b & 0x7F) << sh;
        sh += 7;
        if (!(b & 0x80)) return v;
    }
    r->bad = 1;
    return 0;
}

static uint8_t byte(rd *r) { if (r->p < r->e) return *r->p++; r->bad = 1; return 0; }

static JSValue name(JSContext *ctx, rd *r)
{
    uint32_t n = leb(r);
    if (r->bad || (uint32_t)(r->e - r->p) < n) { r->bad = 1; return JS_NewString(ctx, ""); }
    JSValue s = JS_NewStringLen(ctx, (const char *)r->p, n);
    r->p += n;
    return s;
}

static char letter(uint8_t t) { return t == 0x7F ? 'i' : t == 0x7E ? 'I' : t == 0x7D ? 'f' : t == 0x7C ? 'F' : '?'; }

static void limits(rd *r) { uint8_t f = byte(r); leb(r); if (f & 1) leb(r); }

/* types[i]: "params:results" */
static int read_types(rd *r, char (*types)[64], int max)
{
    uint32_t n = leb(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        if (byte(r) != 0x60) { r->bad = 1; break; }
        char t[64];
        int k = 0;
        uint32_t np = leb(r);
        for (uint32_t j = 0; j < np && !r->bad; j++) { char c = letter(byte(r)); if (k < 60) t[k++] = c; }
        if (k < 61) t[k++] = ':';
        uint32_t nr = leb(r);
        for (uint32_t j = 0; j < nr && !r->bad; j++) { char c = letter(byte(r)); if (k < 62) t[k++] = c; }
        t[k] = 0;
        if ((int)i < max) memcpy(types[i], t, sizeof(t));
    }
    return (int)n;
}

static JSValue describe(JSContext *ctx, const uint8_t *b, uint32_t len, JSValue *imports, JSValue *exports)
{
    static char types[1024][64];
    int ntypes = 0;
    uint32_t func_types[4096];
    int nfuncs = 0;                  /* imported functions first, then the module's own */
    char global_types[1024];
    int nglobals = 0;
    *imports = JS_NewArray(ctx);
    *exports = JS_NewArray(ctx);
    rd r = { b + 8, b + len, 0 };
    int ni = 0, ne = 0;
    while (r.p < r.e && !r.bad) {
        uint8_t id = byte(&r);
        uint32_t sl = leb(&r);
        if (r.bad || (uint32_t)(r.e - r.p) < sl) return JS_ThrowSyntaxError(ctx, "section runs past the end");
        rd s = { r.p, r.p + sl, 0 };
        r.p += sl;
        if (id == 1) ntypes = read_types(&s, types, 1024);
        else if (id == 2) {
            uint32_t n = leb(&s);
            for (uint32_t i = 0; i < n && !s.bad; i++) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "module", name(ctx, &s));
                JS_SetPropertyStr(ctx, o, "name", name(ctx, &s));
                uint8_t kind = byte(&s);
                JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, kind));
                if (kind == 0) {
                    uint32_t ti = leb(&s);
                    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, (int)ti < ntypes && ti < 1024 ? types[ti] : ":"));
                    if (nfuncs < 4096) func_types[nfuncs++] = ti;
                } else if (kind == 1) { byte(&s); limits(&s); }
                else if (kind == 2) limits(&s);
                else if (kind == 3) {
                    char t[2] = { letter(byte(&s)), 0 };
                    byte(&s);
                    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, t));
                    if (nglobals < 1024) global_types[nglobals++] = t[0];
                }
                JS_SetPropertyUint32(ctx, *imports, (uint32_t)ni++, o);
            }
        } else if (id == 3) {
            uint32_t n = leb(&s);
            for (uint32_t i = 0; i < n && !s.bad; i++) { uint32_t ti = leb(&s); if (nfuncs < 4096) func_types[nfuncs++] = ti; }
        } else if (id == 6) {
            uint32_t n = leb(&s);
            for (uint32_t i = 0; i < n && !s.bad; i++) {
                char t = letter(byte(&s));
                byte(&s);
                if (nglobals < 1024) global_types[nglobals++] = t;
                while (s.p < s.e && *s.p != 0x0B) s.p++;      /* the init expression, to its end */
                s.p++;
            }
        } else if (id == 7) {
            uint32_t n = leb(&s);
            for (uint32_t i = 0; i < n && !s.bad; i++) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "name", name(ctx, &s));
                uint8_t kind = byte(&s);
                uint32_t idx = leb(&s);
                JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, kind));
                if (kind == 0 && (int)idx < nfuncs && func_types[idx] < (uint32_t)ntypes && func_types[idx] < 1024)
                    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, types[func_types[idx]]));
                else if (kind == 3 && (int)idx < nglobals) { char t[2] = { global_types[idx], 0 }; JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, t)); }
                JS_SetPropertyUint32(ctx, *exports, (uint32_t)ne++, o);
            }
        }
        if (s.bad) return JS_ThrowSyntaxError(ctx, "section %d is malformed", id);
    }
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ errors */

static JSValue wasm_error(JSContext *ctx, const char *kind, const char *msg, const char *more)
{
    JSValue e = JS_NewError(ctx);
    char m[300];
    snprintf(m, sizeof(m), "%s%s%s", msg ? msg : "error", more && *more ? ": " : "", more ? more : "");
    JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, m));
    JS_SetPropertyStr(ctx, e, "wasmKind", JS_NewString(ctx, kind));
    return JS_Throw(ctx, e);
}

/* ------------------------------------------------------------------ handles */

static void mod_finalizer(JSRuntime *rt, JSValue v)
{
    (void)rt;
    wmod *m = JS_GetOpaque(v, mod_class);
    if (m) { free(m->bytes); free(m); }
}

static void inst_free(JSRuntime *rt, winst *in)
{
    for (int i = 0; i < in->nthunks; i++) JS_FreeValueRT(rt, in->thunks[i].fn);
    free(in->thunks);
    if (in->has_pending) JS_FreeValueRT(rt, in->pending);
    if (!JS_IsUndefined(in->buffer)) JS_FreeValueRT(rt, in->buffer);
    if (in->rt) m3_FreeRuntime(in->rt);
    else if (in->mod) m3_FreeModule(in->mod);
    if (in->env) m3_FreeEnvironment(in->env);
    free(in->bytes);
    free(in);
}

static void inst_finalizer(JSRuntime *rt, JSValue v)
{
    winst *in = JS_GetOpaque(v, inst_class);
    if (in) inst_free(rt, in);
}

static void inst_mark(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark)
{
    winst *in = JS_GetOpaque(v, inst_class);
    if (!in) return;
    for (int i = 0; i < in->nthunks; i++) JS_MarkValue(rt, in->thunks[i].fn, mark);
    if (in->has_pending) JS_MarkValue(rt, in->pending, mark);
    if (!JS_IsUndefined(in->buffer)) JS_MarkValue(rt, in->buffer, mark);
}

/* ------------------------------------------------------------------ values across */

static JSValue to_js(JSContext *ctx, char t, const uint64_t *slot)
{
    int32_t i; int64_t l; float f; double d;
    switch (t) {
    case 'i': memcpy(&i, slot, 4); return JS_NewInt32(ctx, i);
    case 'I': memcpy(&l, slot, 8); return JS_NewBigInt64(ctx, l);
    case 'f': memcpy(&f, slot, 4); return JS_NewFloat64(ctx, (double)f);
    case 'F': memcpy(&d, slot, 8); return JS_NewFloat64(ctx, d);
    }
    return JS_UNDEFINED;
}

static int from_js(JSContext *ctx, char t, JSValueConst v, uint64_t *slot)
{
    *slot = 0;
    if (t == 'i') { int32_t x; if (JS_ToInt32(ctx, &x, v)) return -1; memcpy(slot, &x, 4); return 0; }
    if (t == 'I') {
        int64_t x;
        if (JS_IsNumber(v)) { double d; if (JS_ToFloat64(ctx, &d, v)) return -1; x = (int64_t)d; }
        else if (JS_ToBigInt64(ctx, &x, v)) return -1;
        memcpy(slot, &x, 8);
        return 0;
    }
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    if (t == 'f') { float f = (float)d; memcpy(slot, &f, 4); } else memcpy(slot, &d, 8);
    return 0;
}

/* a value in a slot (as from_js leaves it) into a global */
static void slot_to_global(M3Global *g, char t, const uint64_t *slot)
{
    if (t == 'i') { int32_t x; memcpy(&x, slot, 4); g->intValue = x; }
    else if (t == 'I') memcpy(&g->intValue, slot, 8);
    else if (t == 'f') memcpy(&g->f32Value, slot, 4);
    else memcpy(&g->f64Value, slot, 8);
}

/* an import: the wasm arguments to JS, its answer back */
static const void *trampoline(IM3Runtime runtime, IM3ImportContext ic, uint64_t *sp, void *mem)
{
    (void)mem;
    wthunk *t = ic->userdata;
    winst *in = t->in;
    JSContext *ctx = in->ctx;
    const char *colon = strchr(t->sig, ':');
    int np = (int)(colon - t->sig), nr = (int)strlen(colon + 1);
    JSValue args[32];
    if (np > 32) np = 32;
    for (int i = 0; i < np; i++) args[i] = to_js(ctx, t->sig[i], sp + nr + i);
    /* a call back into wasm from here starts above this frame */
    void *was = runtime->stack;
    runtime->stack = sp + nr + np + 4;
    JSValue r = JS_Call(ctx, t->fn, JS_UNDEFINED, np, args);
    runtime->stack = was;
    for (int i = 0; i < np; i++) JS_FreeValue(ctx, args[i]);
    if (JS_IsException(r)) {
        if (in->has_pending) JS_FreeValue(ctx, in->pending);
        in->pending = JS_GetException(ctx);
        in->has_pending = 1;
        return "tfire: a JavaScript import threw";
    }
    if (nr == 1 && from_js(ctx, colon[1], r, sp)) {
        JS_FreeValue(ctx, r);
        if (in->has_pending) JS_FreeValue(ctx, in->pending);
        in->pending = JS_GetException(ctx);
        in->has_pending = 1;
        return "tfire: a JavaScript import gave back a wrong value";
    }
    if (nr > 1) {
        /* several results: an iterable */
        for (int i = 0; i < nr; i++) {
            JSValue x = JS_GetPropertyUint32(ctx, r, (uint32_t)i);
            from_js(ctx, colon[1 + i], x, sp + i);
            JS_FreeValue(ctx, x);
        }
    }
    JS_FreeValue(ctx, r);
    return m3Err_none;
}

/* wasm3's signature for a type: "i(iI)" */
static void m3sig(const char *t, char *out)
{
    const char *colon = strchr(t, ':');
    int k = 0;
    out[k++] = colon && colon[1] ? colon[1] : 'v';
    out[k++] = '(';
    for (const char *p = t; p < colon && k < 60; p++) out[k++] = *p;
    out[k++] = ')';
    out[k] = 0;
}

/* ------------------------------------------------------------------ the natives */

static JSValue js_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    size_t len = 0;
    uint8_t *b = argc > 0 ? JS_GetArrayBuffer(ctx, &len, argv[0]) : 0;
    if (!b) {
        size_t off = 0, el = 0;
        JSValue ab = argc > 0 ? JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &el) : JS_EXCEPTION;
        if (JS_IsException(ab)) { JS_FreeValue(ctx, JS_GetException(ctx)); return wasm_error(ctx, "compile", "WebAssembly wants an ArrayBuffer or a typed array", 0); }
        size_t al;
        uint8_t *base = JS_GetArrayBuffer(ctx, &al, ab);
        JS_FreeValue(ctx, ab);
        if (!base) return wasm_error(ctx, "compile", "the buffer is detached", 0);
        b = base + off;
    }
    if (len < 8 || memcmp(b, "\0asm", 4) || b[4] != 1) return wasm_error(ctx, "compile", "not a WebAssembly module (version 1)", 0);
    wmod *m = calloc(1, sizeof(*m));
    if (!m || !(m->bytes = malloc(len))) { free(m); return JS_ThrowOutOfMemory(ctx); }
    memcpy(m->bytes, b, len);
    m->len = (uint32_t)len;
    /* wasm3 reads it all, to say now what is wrong with it */
    IM3Environment env = m3_NewEnvironment();
    IM3Module mod = 0;
    M3Result res = env ? m3_ParseModule(env, &mod, m->bytes, m->len) : "no memory";
    if (mod) m3_FreeModule(mod);
    if (env) m3_FreeEnvironment(env);
    if (res) { free(m->bytes); free(m); return wasm_error(ctx, "compile", res, 0); }
    JSValue imports, exports;
    JSValue d = describe(ctx, m->bytes, m->len, &imports, &exports);
    if (JS_IsException(d)) { JS_FreeValue(ctx, imports); JS_FreeValue(ctx, exports); free(m->bytes); free(m); return d; }
    JSValue h = JS_NewObjectClass(ctx, (int)mod_class);
    JS_SetOpaque(h, m);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "mod", h);
    JS_SetPropertyStr(ctx, o, "imports", imports);
    JS_SetPropertyStr(ctx, o, "exports", exports);
    return o;
}

static JSValue js_instantiate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    wmod *m = argc > 0 ? JS_GetOpaque(argv[0], mod_class) : 0;
    if (!m) return JS_ThrowTypeError(ctx, "not a WebAssembly.Module");
    JSValueConst values = argc > 1 ? argv[1] : JS_UNDEFINED;
    winst *in = calloc(1, sizeof(*in));
    if (!in) return JS_ThrowOutOfMemory(ctx);
    in->ctx = ctx;
    in->buffer = JS_UNDEFINED;
    in->pending = JS_UNDEFINED;
    JSValue h = JS_NewObjectClass(ctx, (int)inst_class);
    JS_SetOpaque(h, in);
    M3Result res = 0;
    if (!(in->bytes = malloc(m->len))) { JS_FreeValue(ctx, h); return JS_ThrowOutOfMemory(ctx); }
    memcpy(in->bytes, m->bytes, m->len);
    in->env = m3_NewEnvironment();
    in->rt = in->env ? m3_NewRuntime(in->env, WASM_STACK, in) : 0;
    if (!in->rt) { JS_FreeValue(ctx, h); return JS_ThrowOutOfMemory(ctx); }
    if ((res = m3_ParseModule(in->env, &in->mod, in->bytes, m->len))) { JS_FreeValue(ctx, h); return wasm_error(ctx, "compile", res, 0); }
    if ((res = m3_LoadModule(in->rt, in->mod))) {
        m3_FreeModule(in->mod);
        in->mod = 0;
        JS_FreeValue(ctx, h);
        return wasm_error(ctx, "link", res, 0);
    }
    /* the imports, in the order the module lists them */
    JSValue imports, exports;
    describe(ctx, in->bytes, m->len, &imports, &exports);
    JS_FreeValue(ctx, exports);
    uint32_t ni = 0;
    JSValue lv = JS_GetPropertyStr(ctx, imports, "length");
    JS_ToUint32(ctx, &ni, lv);
    JS_FreeValue(ctx, lv);
    if (ni > MAX_IMPORTS) ni = MAX_IMPORTS;
    in->thunks = calloc(ni ? ni : 1, sizeof(wthunk));
    uint32_t global_i = 0;
    for (uint32_t i = 0; i < ni && !res; i++) {
        JSValue im = JS_GetPropertyUint32(ctx, imports, i);
        JSValue v = JS_GetPropertyUint32(ctx, values, i);
        JSValue jk = JS_GetPropertyStr(ctx, im, "kind");
        int32_t kind = 0;
        JS_ToInt32(ctx, &kind, jk);
        const char *mn = 0, *fn = 0, *ty = 0;
        JSValue jm = JS_GetPropertyStr(ctx, im, "module"), jf = JS_GetPropertyStr(ctx, im, "name"), jt = JS_GetPropertyStr(ctx, im, "type");
        mn = JS_ToCString(ctx, jm);
        fn = JS_ToCString(ctx, jf);
        ty = JS_IsUndefined(jt) ? 0 : JS_ToCString(ctx, jt);
        if (kind == 0 && mn && fn && ty) {
            if (!JS_IsFunction(ctx, v)) res = "an imported function is not a function";
            else {
                wthunk *t = &in->thunks[in->nthunks++];
                t->in = in;
                t->fn = JS_DupValue(ctx, v);
                strncpy(t->sig, ty, sizeof(t->sig) - 1);
                char sig[72];
                m3sig(ty, sig);
                M3Result lr = m3_LinkRawFunctionEx(in->mod, mn, fn, sig, trampoline, t);
                if (lr && lr != m3Err_functionLookupFailed) res = lr;
            }
        } else if (kind == 2) {
            /* an imported memory: what JS wrote in it before goes into the instance's own */
            size_t bl = 0;
            uint8_t *src = JS_GetArrayBuffer(ctx, &bl, v);
            if (!src) JS_FreeValue(ctx, JS_GetException(ctx));
            uint32_t ml = 0;
            uint8_t *dst = m3_GetMemory(in->rt, &ml, 0);
            if (src && dst && bl) memcpy(dst, src, bl < ml ? bl : ml);
        } else if (kind == 3 && ty) {
            /* an imported global: its value, in the module's imported globals in order */
            uint32_t seen = 0;
            for (uint32_t g = 0; g < in->mod->numGlobals; g++) {
                M3Global *gl = &in->mod->globals[g];
                if (!gl->imported) continue;
                if (seen++ != global_i) continue;
                uint64_t slot;
                if (!from_js(ctx, ty[0], v, &slot)) slot_to_global(gl, ty[0], &slot);
                else JS_FreeValue(ctx, JS_GetException(ctx));
                break;
            }
            global_i++;
        }
        if (mn) JS_FreeCString(ctx, mn);
        if (fn) JS_FreeCString(ctx, fn);
        if (ty) JS_FreeCString(ctx, ty);
        JS_FreeValue(ctx, jm); JS_FreeValue(ctx, jf); JS_FreeValue(ctx, jt); JS_FreeValue(ctx, jk);
        JS_FreeValue(ctx, im); JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, imports);
    if (res) { JS_FreeValue(ctx, h); return wasm_error(ctx, "link", res, 0); }
    if ((res = m3_RunStart(in->mod))) {
        JSValue e = in->has_pending ? JS_Throw(ctx, JS_DupValue(ctx, in->pending)) : wasm_error(ctx, "runtime", res, 0);
        JS_FreeValue(ctx, h);
        return e;
    }
    return h;
}

static JSValue js_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    winst *in = argc > 0 ? JS_GetOpaque(argv[0], inst_class) : 0;
    const char *nm = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    if (!in || !nm) { if (nm) JS_FreeCString(ctx, nm); return JS_ThrowTypeError(ctx, "bad WebAssembly call"); }
    IM3Function f = 0;
    M3Result res = m3_FindFunction(&f, in->rt, nm);
    JS_FreeCString(ctx, nm);
    if (res) return wasm_error(ctx, "runtime", res, 0);
    uint32_t na = m3_GetArgCount(f), nr = m3_GetRetCount(f);
    if (na > 32 || nr > 16) return wasm_error(ctx, "runtime", "too many arguments or results", 0);
    uint64_t vals[32];
    const void *ptrs[32];
    for (uint32_t i = 0; i < na; i++) {
        M3ValueType t = m3_GetArgType(f, i);
        char c = t == c_m3Type_i32 ? 'i' : t == c_m3Type_i64 ? 'I' : t == c_m3Type_f32 ? 'f' : 'F';
        JSValue a = argc > 2 ? JS_GetPropertyUint32(ctx, argv[2], i) : JS_UNDEFINED;
        int bad = from_js(ctx, c, a, &vals[i]);
        JS_FreeValue(ctx, a);
        if (bad) return JS_EXCEPTION;
        ptrs[i] = &vals[i];
    }
    if (in->has_pending) { JS_FreeValue(ctx, in->pending); in->has_pending = 0; }
    res = m3_Call(f, na, ptrs);
    if (res) {
        if (in->has_pending) { in->has_pending = 0; JSValue p = in->pending; in->pending = JS_UNDEFINED; return JS_Throw(ctx, p); }
        M3ErrorInfo info;
        m3_GetErrorInfo(in->rt, &info);
        return wasm_error(ctx, "runtime", res, info.message);
    }
    if (!nr) return JS_UNDEFINED;
    uint64_t rv[16];
    const void *rp[16];
    for (uint32_t i = 0; i < nr; i++) { rv[i] = 0; rp[i] = &rv[i]; }
    if ((res = m3_GetResults(f, nr, rp))) return wasm_error(ctx, "runtime", res, 0);
    JSValue out = nr > 1 ? JS_NewArray(ctx) : JS_UNDEFINED;
    for (uint32_t i = 0; i < nr; i++) {
        M3ValueType t = m3_GetRetType(f, i);
        char c = t == c_m3Type_i32 ? 'i' : t == c_m3Type_i64 ? 'I' : t == c_m3Type_f32 ? 'f' : 'F';
        JSValue v = to_js(ctx, c, &rv[i]);
        if (nr == 1) return v;
        JS_SetPropertyUint32(ctx, out, i, v);
    }
    return out;
}

static void no_free(JSRuntime *rt, void *opaque, void *ptr) { (void)rt; (void)opaque; (void)ptr; }

static JSValue js_memory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    winst *in = argc > 0 ? JS_GetOpaque(argv[0], inst_class) : 0;
    if (!in) return JS_ThrowTypeError(ctx, "not a WebAssembly.Instance");
    uint32_t len = 0;
    uint8_t *at = m3_GetMemory(in->rt, &len, 0);
    if (!at) return JS_NULL;
    if (JS_IsUndefined(in->buffer) || at != in->buf_at || len != in->buf_len) {
        /* the memory moved or grew: the old buffer may no longer be read through */
        if (!JS_IsUndefined(in->buffer)) { JS_DetachArrayBuffer(ctx, in->buffer); JS_FreeValue(ctx, in->buffer); }
        in->buffer = JS_NewArrayBuffer(ctx, at, len, no_free, 0, 0);
        in->buf_at = at;
        in->buf_len = len;
    }
    return JS_DupValue(ctx, in->buffer);
}

static JSValue js_grow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    winst *in = argc > 0 ? JS_GetOpaque(argv[0], inst_class) : 0;
    int32_t d = 0;
    if (!in || argc < 2 || JS_ToInt32(ctx, &d, argv[1]) || d < 0) return JS_NewInt32(ctx, -1);
    uint32_t old = in->rt->memory.numPages;
    if (d && ResizeMemory(in->rt, old + (uint32_t)d)) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, (int32_t)old);
}

static JSValue js_global(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    winst *in = argc > 0 ? JS_GetOpaque(argv[0], inst_class) : 0;
    const char *nm = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    if (!in || !nm) { if (nm) JS_FreeCString(ctx, nm); return JS_ThrowTypeError(ctx, "bad WebAssembly global"); }
    IM3Global g = m3_FindGlobal(in->mod, nm);
    JS_FreeCString(ctx, nm);
    if (!g) return JS_UNDEFINED;
    char c = g->type == c_m3Type_i32 ? 'i' : g->type == c_m3Type_i64 ? 'I' : g->type == c_m3Type_f32 ? 'f' : 'F';
    if (argc > 2) {
        uint64_t slot;
        if (from_js(ctx, c, argv[2], &slot)) return JS_EXCEPTION;
        slot_to_global(g, c, &slot);
    }
    uint64_t slot = 0;
    if (c == 'i') { int32_t x = (int32_t)g->intValue; memcpy(&slot, &x, 4); }
    else if (c == 'I') memcpy(&slot, &g->intValue, 8);
    else if (c == 'f') memcpy(&slot, &g->f32Value, 4);
    else memcpy(&slot, &g->f64Value, 8);
    return to_js(ctx, c, &slot);
}

static const JSCFunctionListEntry wasm_natives[] = {
    JS_CFUNC_DEF("wasmCompile", 1, js_compile),
    JS_CFUNC_DEF("wasmInstantiate", 2, js_instantiate),
    JS_CFUNC_DEF("wasmCall", 3, js_call),
    JS_CFUNC_DEF("wasmMemory", 1, js_memory),
    JS_CFUNC_DEF("wasmGrow", 2, js_grow),
    JS_CFUNC_DEF("wasmGlobal", 3, js_global),
};

void tfwasm_install(JSContext *ctx, JSValue tf)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!mod_class) { JS_NewClassID(&mod_class); JS_NewClassID(&inst_class); }
    JSClassDef md = { "WasmModuleHandle", mod_finalizer, 0, 0, 0 };
    JSClassDef id = { "WasmInstanceHandle", inst_finalizer, inst_mark, 0, 0 };
    JS_NewClass(rt, mod_class, &md);
    JS_NewClass(rt, inst_class, &id);
    JS_SetPropertyFunctionList(ctx, tf, wasm_natives, sizeof(wasm_natives) / sizeof(wasm_natives[0]));
}
