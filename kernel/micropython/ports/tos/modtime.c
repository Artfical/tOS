/* time module for tOS MicroPython */
#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include "py/mphal.h"
#include "../../core/debugmon.h"
#include "../../core/scheduler.h"

/* time.ticks_ms() — ms since boot */
static mp_obj_t mp_time_ticks_ms(void) {
    return mp_obj_new_int((mp_int_t)debugmon_uptime_ms());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_time_ticks_ms_obj, mp_time_ticks_ms);

/* time.ticks_us() — µs since boot (approximate: ms * 1000) */
static mp_obj_t mp_time_ticks_us(void) {
    return mp_obj_new_int((mp_int_t)(debugmon_uptime_ms() * 1000));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_time_ticks_us_obj, mp_time_ticks_us);

/* time.ticks_diff(end, start) */
static mp_obj_t mp_time_ticks_diff(mp_obj_t end_in, mp_obj_t start_in) {
    mp_int_t d = mp_obj_get_int(end_in) - mp_obj_get_int(start_in);
    return mp_obj_new_int(d);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_time_ticks_diff_obj, mp_time_ticks_diff);

/* time.time() — seconds since boot (no RTC) */
static mp_obj_t mp_time_time(void) {
    return mp_obj_new_int((mp_int_t)(debugmon_uptime_ms() / 1000));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_time_time_obj, mp_time_time);

/* time.sleep(seconds) */
static mp_obj_t mp_time_sleep(mp_obj_t s_in) {
    mp_int_t ms = (mp_int_t)(mp_obj_get_float(s_in) * 1000.0f);
    mp_hal_delay_ms((mp_uint_t)ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_time_sleep_obj, mp_time_sleep);

/* time.sleep_ms(ms) */
static mp_obj_t mp_time_sleep_ms(mp_obj_t ms_in) {
    mp_hal_delay_ms((mp_uint_t)mp_obj_get_int(ms_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_time_sleep_ms_obj, mp_time_sleep_ms);

/* time.sleep_us(us) */
static mp_obj_t mp_time_sleep_us(mp_obj_t us_in) {
    mp_hal_delay_us((mp_uint_t)mp_obj_get_int(us_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_time_sleep_us_obj, mp_time_sleep_us);

static void time_store(mp_obj_dict_t *g, const char *name, const void *fn) {
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g),
        MP_OBJ_NEW_QSTR(qstr_from_str(name)), MP_OBJ_FROM_PTR(fn));
}

void time_module_init(void) {
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("time"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);

    time_store(g, "ticks_ms",   &mp_time_ticks_ms_obj);
    time_store(g, "ticks_us",   &mp_time_ticks_us_obj);
    time_store(g, "ticks_diff", &mp_time_ticks_diff_obj);
    time_store(g, "time",       &mp_time_time_obj);
    time_store(g, "sleep",      &mp_time_sleep_obj);
    time_store(g, "sleep_ms",   &mp_time_sleep_ms_obj);
    time_store(g, "sleep_us",   &mp_time_sleep_us_obj);
}
