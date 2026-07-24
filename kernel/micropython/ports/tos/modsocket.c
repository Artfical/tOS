/* socket module for tOS MicroPython — UDP + TCP backed by kernel net stack */
#include "py/obj.h"
#include "py/runtime.h"
#include "py/qstr.h"
#include <string.h>
#include "../../net/net.h"
#include "../../net/udp.h"
#include "../../net/tcp.h"
#include "../../net/dns.h"
#include "../../net/tls.h"

/* parse "a.b.c.d" into four unsigned ints; returns 4 on success */
static int parse_ipv4(const char *s, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    *a = *b = *c = *d = 0;
    unsigned *parts[4] = { a, b, c, d };
    int part = 0;
    while (*s && part < 4) {
        if (*s >= '0' && *s <= '9') {
            *parts[part] = *parts[part] * 10 + (unsigned)(*s - '0');
        } else if (*s == '.') {
            part++;
        } else {
            return 0;
        }
        s++;
    }
    return (part == 3) ? 4 : 0;
}

/* write uint32 as decimal into buf; returns number of chars written */
static int uint_to_str(char *buf, unsigned v) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12]; int n = 0;
    while (v) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

static void format_ip(char *out, uint32_t ip) {
    int n = 0;
    unsigned oct[4] = { ip & 0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF };
    for (int i = 0; i < 4; i++) {
        n += uint_to_str(out + n, oct[i]);
        if (i < 3) out[n++] = '.';
    }
    out[n] = '\0';
}

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* ---- socket object ---- */
typedef struct {
    mp_obj_base_t base;
    int type;        /* SOCK_STREAM or SOCK_DGRAM */
    uint16_t port;   /* local port (UDP) */
    int connected;   /* TCP only */
    int use_tls;     /* socket.socket(..., tls=True) -- see mp_socket_socket */
} tos_sock_obj_t;

/* forward declaration — defined via MP_DEFINE_CONST_OBJ_TYPE below */
extern const mp_obj_type_t tos_sock_type;

/* One shared TLS context, like https.c's g_tls -- tls_ctx_t carries
 * ~24KB of handshake/record buffers, too much to heap-allocate per
 * socket on a 128KB total heap. Fine for this port's actual usage:
 * one TLS connection open at a time (git.py's clone/push, sequentially). */
static tls_ctx_t g_tls_sock;

/* resolve hostname or dotted-decimal string to uint32_t IP */
static uint32_t resolve_host(const char *host) {
    /* try dotted-decimal first */
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (parse_ipv4(host, &a, &b, &c, &d) == 4)
        return IP4(a, b, c, d);
    uint32_t ip = 0;
    dns_resolve(host, &ip);
    return ip;
}

/* socket.connect((host, port)) */
static mp_obj_t sock_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t *items; size_t n;
    mp_obj_get_array(addr_in, &n, &items);
    if (n < 2) mp_raise_TypeError(MP_ERROR_TEXT("addr must be (host, port)"));
    const char *host = mp_obj_str_get_str(items[0]);
    uint16_t port    = (uint16_t)mp_obj_get_int(items[1]);

    if (self->type == SOCK_STREAM) {
        uint32_t ip = resolve_host(host);
        int r;
        if (self->use_tls) {
            r = tls_connect(&g_tls_sock, ip, port, host);
        } else {
            r = tcp_connect(ip, port);
        }
        if (r < 0) mp_raise_OSError(-r);
        self->connected = 1;
    } else {
        /* UDP: just remember remote addr would need sendto; connect() on UDP
           is "default destination" — we store it as local port */
        self->port = port;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

/* socket.bind((host, port)) — UDP listen port */
static mp_obj_t sock_bind(mp_obj_t self_in, mp_obj_t addr_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t *items; size_t n;
    mp_obj_get_array(addr_in, &n, &items);
    if (n < 2) mp_raise_TypeError(MP_ERROR_TEXT("addr must be (host, port)"));
    self->port = (uint16_t)mp_obj_get_int(items[1]);
    udp_open(self->port);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_bind_obj, sock_bind);

/* socket.send(data) — TCP */
static mp_obj_t sock_send(mp_obj_t self_in, mp_obj_t data_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->type != SOCK_STREAM) mp_raise_OSError(38); /* ENOSYS */
    size_t len; const char *data = mp_obj_str_get_data(data_in, &len);
    int r = self->use_tls
        ? tls_write(&g_tls_sock, (const uint8_t *)data, (int)len)
        : tcp_send((void *)data, (int)len);
    if (r < 0) mp_raise_OSError(-r);
    /* tls_write() returns 0 on success (all-or-nothing), unlike
     * tcp_send()'s byte count -- report the full length either way so
     * callers checking "did everything go out" see a consistent API. */
    return mp_obj_new_int(self->use_tls ? (int)len : r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_send_obj, sock_send);

/* socket.recv(bufsize) — TCP */
static mp_obj_t sock_recv(mp_obj_t self_in, mp_obj_t size_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->type != SOCK_STREAM) mp_raise_OSError(38);
    int sz = mp_obj_get_int(size_in);
    if (sz <= 0 || sz > 65536) sz = 4096;
    uint8_t *buf = (uint8_t *)m_malloc((size_t)sz);
    if (!buf) mp_raise_OSError(12);
    int n = self->use_tls ? tls_read(&g_tls_sock, buf, sz) : tcp_recv(buf, sz);
    if (n < 0) { m_free(buf); mp_raise_OSError(-n); }
    mp_obj_t ret = mp_obj_new_str((char *)buf, (size_t)n);
    m_free(buf);
    return ret;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recv_obj, sock_recv);

/* socket.sendto(data, (host, port)) — UDP */
static mp_obj_t sock_sendto(size_t n_args, const mp_obj_t *args) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->type != SOCK_DGRAM) mp_raise_OSError(38);
    size_t dlen; const char *data = mp_obj_str_get_data(args[1], &dlen);
    mp_obj_t *addr; size_t alen;
    mp_obj_get_array(args[2], &alen, &addr);
    if (alen < 2) mp_raise_TypeError(MP_ERROR_TEXT("addr must be (host, port)"));
    const char *host = mp_obj_str_get_str(addr[0]);
    uint16_t dport   = (uint16_t)mp_obj_get_int(addr[1]);
    uint32_t ip      = resolve_host(host);
    uint16_t sport   = self->port ? self->port : 49152;
    int r = udp_send(ip, dport, sport, (void *)data, (int)dlen);
    if (r < 0) mp_raise_OSError(-r);
    return mp_obj_new_int((mp_int_t)dlen);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_sendto_obj, 3, 3, sock_sendto);

/* socket.recvfrom(bufsize) — UDP, returns (data, (ip_str, port)) */
static mp_obj_t sock_recvfrom(mp_obj_t self_in, mp_obj_t size_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->type != SOCK_DGRAM) mp_raise_OSError(38);
    int sz = mp_obj_get_int(size_in);
    if (sz <= 0 || sz > 65536) sz = 4096;
    uint8_t *buf = (uint8_t *)m_malloc((size_t)sz);
    if (!buf) mp_raise_OSError(12);
    uint32_t src_ip = 0; uint16_t src_port = 0;
    int n = udp_listen(self->port, buf, sz, &src_ip, &src_port);
    if (n < 0) { m_free(buf); mp_raise_OSError(-n); }
    mp_obj_t data = mp_obj_new_str((char *)buf, (size_t)n);
    m_free(buf);
    /* format IP string */
    char ip_s[20];
    format_ip(ip_s, src_ip);
    mp_obj_t addr_items[2] = { mp_obj_new_str(ip_s, strlen(ip_s)),
                                mp_obj_new_int(src_port) };
    return mp_obj_new_tuple(2, (mp_obj_t[]){ data, mp_obj_new_tuple(2, addr_items) });
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recvfrom_obj, sock_recvfrom);

/* socket.close() */
static mp_obj_t sock_close(mp_obj_t self_in) {
    tos_sock_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->type == SOCK_STREAM && self->connected) {
        if (self->use_tls) {
            tls_close(&g_tls_sock);
        } else {
            tcp_close();
        }
        self->connected = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_close_obj, sock_close);

static mp_obj_t sock_enter(mp_obj_t self_in) { return self_in; }
static MP_DEFINE_CONST_FUN_OBJ_1(sock_enter_obj, sock_enter);
static mp_obj_t sock_exit(size_t n, const mp_obj_t *args) { (void)n; return sock_close(args[0]); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_exit_obj, 4, 4, sock_exit);

/* This is the type's real locals dict -- what MicroPython's attribute
 * lookup actually consults for `sock.connect(...)`-style calls. The
 * constructor used to build a fresh per-instance dict with these same
 * entries and then just discard it (`(void)d`), so none of these were
 * ever reachable via normal method-call syntax -- only a direct,
 * unbound call like `sock_connect_obj(sock, addr)` would have worked,
 * which nothing (including this port's own test code) ever did. */
static const mp_rom_map_elem_t sock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&sock_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&sock_exit_obj)  },
    { MP_ROM_QSTR(MP_QSTR_connect),   MP_ROM_PTR(&sock_connect_obj)  },
    { MP_ROM_QSTR(MP_QSTR_bind),      MP_ROM_PTR(&sock_bind_obj)     },
    { MP_ROM_QSTR(MP_QSTR_send),      MP_ROM_PTR(&sock_send_obj)     },
    { MP_ROM_QSTR(MP_QSTR_recv),      MP_ROM_PTR(&sock_recv_obj)     },
    { MP_ROM_QSTR(MP_QSTR_sendto),    MP_ROM_PTR(&sock_sendto_obj)   },
    { MP_ROM_QSTR(MP_QSTR_recvfrom),  MP_ROM_PTR(&sock_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&sock_close_obj)    },
};
static MP_DEFINE_CONST_DICT(sock_locals_dict, sock_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    tos_sock_type,
    MP_QSTR_object,
    MP_TYPE_FLAG_NONE,
    locals_dict, &sock_locals_dict
);

/* socket.socket(af, type, tls=False) constructor -- the tls flag is a
 * tOS-specific extension (real Python wraps a plain socket in
 * ssl.wrap_socket() instead); simplest thing that works given this
 * port's tls_connect() folds the TCP connect and TLS handshake into
 * one call, so there's no separate "upgrade this socket" step. */
static mp_obj_t mp_socket_socket(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int af   = (n_args > 0) ? mp_obj_get_int(args[0]) : AF_INET;
    int type = (n_args > 1) ? mp_obj_get_int(args[1]) : SOCK_STREAM;
    int tls  = (n_args > 2) ? mp_obj_is_true(args[2]) : 0;
    (void)af;
    tos_sock_obj_t *s = m_new_obj(tos_sock_obj_t);
    s->base.type = &tos_sock_type;
    s->type      = type;
    s->port      = 0;
    s->connected = 0;
    s->use_tls   = tls;
    return MP_OBJ_FROM_PTR(s);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_socket_socket_obj, 0, 3, mp_socket_socket);

/* socket.getaddrinfo(host, port) -> [(AF_INET, SOCK_STREAM, 0, '', (ip, port))] */
static mp_obj_t mp_socket_getaddrinfo(mp_obj_t host_in, mp_obj_t port_in) {
    const char *host = mp_obj_str_get_str(host_in);
    int port = mp_obj_get_int(port_in);
    uint32_t ip = resolve_host(host);
    char ip_s[20];
    format_ip(ip_s, ip);
    mp_obj_t addr[2] = { mp_obj_new_str(ip_s, strlen(ip_s)),
                         mp_obj_new_int(port) };
    mp_obj_t inner[5] = {
        mp_obj_new_int(AF_INET),
        mp_obj_new_int(SOCK_STREAM),
        mp_obj_new_int(0),
        mp_obj_new_str("", 0),
        mp_obj_new_tuple(2, addr)
    };
    mp_obj_t outer[1] = { mp_obj_new_tuple(5, inner) };
    return mp_obj_new_list(1, outer);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_socket_getaddrinfo_obj, mp_socket_getaddrinfo);

static void sock_store(mp_obj_dict_t *g, const char *name, const void *fn) {
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g),
        MP_OBJ_NEW_QSTR(qstr_from_str(name)), MP_OBJ_FROM_PTR(fn));
}

void socket_module_init(void) {
    mp_obj_t mod = mp_obj_new_module(qstr_from_str("socket"));
    mp_obj_dict_t *g = mp_obj_module_get_globals(mod);

    sock_store(g, "socket",      &mp_socket_socket_obj);
    sock_store(g, "getaddrinfo", &mp_socket_getaddrinfo_obj);

    /* constants */
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g), MP_OBJ_NEW_QSTR(qstr_from_str("AF_INET")),     mp_obj_new_int(AF_INET));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g), MP_OBJ_NEW_QSTR(qstr_from_str("SOCK_STREAM")), mp_obj_new_int(SOCK_STREAM));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(g), MP_OBJ_NEW_QSTR(qstr_from_str("SOCK_DGRAM")),  mp_obj_new_int(SOCK_DGRAM));
}
