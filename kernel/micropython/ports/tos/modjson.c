/* json module for tOS MicroPython — minimal encode/decode */
#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- encoder ---- */
static void json_encode_obj(mp_obj_t obj, char *buf, size_t *pos, size_t max);

static void json_append(char *buf, size_t *pos, size_t max, const char *s, size_t n) {
    for (size_t i = 0; i < n && *pos < max - 1; i++)
        buf[(*pos)++] = s[i];
}

static void json_append_str(char *buf, size_t *pos, size_t max, const char *s, size_t n) {
    json_append(buf, pos, max, "\"", 1);
    for (size_t i = 0; i < n && *pos < max - 2; i++) {
        char c = s[i];
        if      (c == '"')  { json_append(buf, pos, max, "\\\"", 2); }
        else if (c == '\\') { json_append(buf, pos, max, "\\\\", 2); }
        else if (c == '\n') { json_append(buf, pos, max, "\\n",  2); }
        else if (c == '\r') { json_append(buf, pos, max, "\\r",  2); }
        else if (c == '\t') { json_append(buf, pos, max, "\\t",  2); }
        else                { buf[(*pos)++] = c; }
    }
    json_append(buf, pos, max, "\"", 1);
}

static void json_encode_obj(mp_obj_t obj, char *buf, size_t *pos, size_t max) {
    if (obj == mp_const_none) {
        json_append(buf, pos, max, "null", 4);
    } else if (obj == mp_const_true) {
        json_append(buf, pos, max, "true", 4);
    } else if (obj == mp_const_false) {
        json_append(buf, pos, max, "false", 5);
    } else if (mp_obj_is_int(obj)) {
        char tmp[24];
        int n = (int)mp_obj_get_int(obj);
        size_t l = (size_t)snprintf(tmp, sizeof(tmp), "%d", n);
        json_append(buf, pos, max, tmp, l);
    } else if (mp_obj_is_float(obj)) {
        char tmp[32];
        mp_float_t f = mp_obj_get_float(obj);
        size_t l = (size_t)snprintf(tmp, sizeof(tmp), "%g", (double)f);
        json_append(buf, pos, max, tmp, l);
    } else if (mp_obj_is_str(obj)) {
        size_t len; const char *s = mp_obj_str_get_data(obj, &len);
        json_append_str(buf, pos, max, s, len);
    } else if (mp_obj_is_type(obj, &mp_type_list) || mp_obj_is_type(obj, &mp_type_tuple)) {
        size_t len; mp_obj_t *items;
        mp_obj_get_array(obj, &len, &items);
        json_append(buf, pos, max, "[", 1);
        for (size_t i = 0; i < len; i++) {
            if (i) json_append(buf, pos, max, ",", 1);
            json_encode_obj(items[i], buf, pos, max);
        }
        json_append(buf, pos, max, "]", 1);
    } else if (mp_obj_is_type(obj, &mp_type_dict)) {
        mp_map_t *map = mp_obj_dict_get_map(obj);
        json_append(buf, pos, max, "{", 1);
        int first = 1;
        for (size_t i = 0; i < map->alloc; i++) {
            if (!mp_map_slot_is_filled(map, i)) continue;
            if (!first) json_append(buf, pos, max, ",", 1);
            first = 0;
            /* key must be a string */
            size_t klen; const char *ks = mp_obj_str_get_data(map->table[i].key, &klen);
            json_append_str(buf, pos, max, ks, klen);
            json_append(buf, pos, max, ":", 1);
            json_encode_obj(map->table[i].value, buf, pos, max);
        }
        json_append(buf, pos, max, "}", 1);
    } else {
        json_append(buf, pos, max, "null", 4);
    }
}

static mp_obj_t mp_json_dumps(mp_obj_t obj) {
    static char buf[8192];
    size_t pos = 0;
    json_encode_obj(obj, buf, &pos, sizeof(buf));
    buf[pos] = '\0';
    return mp_obj_new_str(buf, pos);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_json_dumps_obj, mp_json_dumps);

/* ---- decoder ---- */
static mp_obj_t json_decode_value(const char *s, size_t *i, size_t len);

static void skip_ws(const char *s, size_t *i, size_t len) {
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n'))
        (*i)++;
}

static mp_obj_t json_decode_string(const char *s, size_t *i, size_t len) {
    (*i)++; /* skip " */
    static char tmp[2048];
    size_t n = 0;
    while (*i < len && s[*i] != '"') {
        char c = s[(*i)++];
        if (c == '\\' && *i < len) {
            c = s[(*i)++];
            if      (c == 'n')  c = '\n';
            else if (c == 'r')  c = '\r';
            else if (c == 't')  c = '\t';
        }
        if (n < sizeof(tmp) - 1) tmp[n++] = c;
    }
    if (*i < len) (*i)++; /* skip closing " */
    return mp_obj_new_str(tmp, n);
}

static mp_obj_t json_decode_value(const char *s, size_t *i, size_t len) {
    skip_ws(s, i, len);
    if (*i >= len) return mp_const_none;

    char c = s[*i];
    if (c == '"') {
        return json_decode_string(s, i, len);
    } else if (c == '{') {
        (*i)++;
        mp_obj_t dict = mp_obj_new_dict(4);
        skip_ws(s, i, len);
        while (*i < len && s[*i] != '}') {
            skip_ws(s, i, len);
            mp_obj_t key = json_decode_string(s, i, len);
            skip_ws(s, i, len);
            if (*i < len && s[*i] == ':') (*i)++;
            mp_obj_t val = json_decode_value(s, i, len);
            mp_obj_dict_store(dict, key, val);
            skip_ws(s, i, len);
            if (*i < len && s[*i] == ',') (*i)++;
        }
        if (*i < len) (*i)++; /* } */
        return dict;
    } else if (c == '[') {
        (*i)++;
        mp_obj_t list = mp_obj_new_list(0, NULL);
        skip_ws(s, i, len);
        while (*i < len && s[*i] != ']') {
            mp_obj_t val = json_decode_value(s, i, len);
            mp_obj_list_append(list, val);
            skip_ws(s, i, len);
            if (*i < len && s[*i] == ',') (*i)++;
        }
        if (*i < len) (*i)++; /* ] */
        return list;
    } else if (strncmp(s + *i, "true", 4) == 0)  { *i += 4; return mp_const_true; }
    else if (strncmp(s + *i, "false", 5) == 0) { *i += 5; return mp_const_false; }
    else if (strncmp(s + *i, "null", 4) == 0)  { *i += 4; return mp_const_none; }
    else {
        /* number */
        size_t start = *i;
        int is_float = 0;
        if (s[*i] == '-') (*i)++;
        while (*i < len && ((s[*i] >= '0' && s[*i] <= '9') || s[*i] == '.' || s[*i] == 'e' || s[*i] == 'E' || s[*i] == '+' || s[*i] == '-')) {
            if (s[*i] == '.' || s[*i] == 'e' || s[*i] == 'E') is_float = 1;
            (*i)++;
        }
        char tmp[32];
        size_t n = *i - start;
        if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
        strncpy(tmp, s + start, n); tmp[n] = '\0';
        if (is_float) return mp_obj_new_float((mp_float_t)atoi(tmp)); /* approx */
        return mp_obj_new_int((mp_int_t)atoi(tmp));
    }
}

static mp_obj_t mp_json_loads(mp_obj_t s_in) {
    size_t len; const char *s = mp_obj_str_get_data(s_in, &len);
    size_t i = 0;
    return json_decode_value(s, &i, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_json_loads_obj, mp_json_loads);

void json_module_init(void) {
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("json"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g), MP_OBJ_NEW_QSTR(qstr_from_str("dumps")), MP_OBJ_FROM_PTR(&mp_json_dumps_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g), MP_OBJ_NEW_QSTR(qstr_from_str("loads")), MP_OBJ_FROM_PTR(&mp_json_loads_obj));
}
