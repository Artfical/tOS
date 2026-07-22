#include "tos_api.h"
#include "shell.h"
#include "fsbridge.h"
#include "vfs.h"
#include "wm.h"
#include "scheduler.h"
#include "string.h"
#include "http.h"
#include "dns.h"
#include "memory.h"
#include "debugmon.h"
#include "audio.h"
#include "wav_decoder.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "m4a_demux.h"

int tos_exec(const char *cmd, char *out, int out_max)
{
    if (out && out_max > 0) {
        shell_exec_capture(cmd, out, out_max);
    } else {
        char scratch[4];
        shell_exec_capture(cmd, scratch, sizeof(scratch));
    }
    return 0;
}

int tos_read(const char *path, char *out, int out_max)
{
    if (out_max <= 0) return -1;
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) { out[0] = 0; return -1; }

    uint32_t sz = fsbridge_size(path);
    if ((int)sz > out_max - 1) sz = (uint32_t)(out_max - 1);

    int n = fsbridge_read(path, out, sz, 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

int tos_write(const char *path, const char *data, int len)
{
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0) return -1;
    if (len > 0 && fsbridge_write(path, data, (uint32_t)len, 0) < 0) return -1;
    return 0;
}

int tos_mkdir(const char *path) { return fsbridge_mkdir(path); }
int tos_delete(const char *path) { return fsbridge_delete(path); }
int tos_exists(const char *path) { return fsbridge_exists(path); }

int tos_list(const char *path, char *out, int out_max)
{
    if (out_max <= 0) return -1;
    out[0] = 0;

    vfs_entry_t entries[128];
    int n = fsbridge_list(path, entries, 128);
    if (n < 0) return -1;

    int k = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        int j = 0;
        while (entries[i].name[j] && k < out_max - 1) out[k++] = entries[i].name[j++];
        if (k < out_max - 1) out[k++] = '\n';
    }
    out[k < out_max ? k : out_max - 1] = 0;
    return n;
}

int tos_open_app(const char *name)
{
    return wm_open_app(name);
}

static char *ps_out;
static int ps_cap;
static int ps_len;

static void ps_append(const char *s)
{
    while (*s && ps_len < ps_cap - 1) ps_out[ps_len++] = *s++;
}

static void ps_append_uint(uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { ps_append("0"); return; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    char buf[12];
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    ps_append(buf);
}

static const char *ps_state_name(uint32_t state)
{
    switch (state) {
        case TASK_STATE_READY: return "READY";
        case TASK_STATE_RUNNING: return "RUN";
        case TASK_STATE_SLEEPING: return "SLEEP";
        case TASK_STATE_ZOMBIE: return "ZOMBIE";
        default: return "?";
    }
}

static void ps_collect(uint32_t pid, const char *name, uint32_t state)
{
    ps_append_uint(pid);
    ps_append(" ");
    ps_append(name);
    ps_append(" ");
    ps_append(ps_state_name(state));
    ps_append("\n");
}

int tos_ps(char *out, int out_max)
{
    if (out_max <= 0) return -1;
    ps_out = out;
    ps_cap = out_max;
    ps_len = 0;
    out[0] = 0;
    task_foreach(ps_collect);
    out[ps_len < ps_cap ? ps_len : ps_cap - 1] = 0;
    return 0;
}

int tos_kill(uint32_t pid)
{
    if (pid == 0) return -1;
    if (pid == task_get_pid()) return -1;
    if (wm_kill_task_window(pid) == 0) return 0;
    return task_kill(pid);
}

uint32_t tos_uptime(void)
{
    /* debugmon_uptime_ms() (PIT-register-calibrated TSC, see
     * debugmon.c) instead of task_get_ticks()/100 -- system_ticks
     * advances on every task_yield() software self-yield as well as
     * real IRQ0 ticks, so its rate depends on scheduling load rather
     * than real wall-clock time. */
    return debugmon_uptime_ms() / 1000;
}

int tos_http_get(const char *url, char *out, int out_max)
{
    if (out_max <= 0) return -1;
    out[0] = 0;
    if (strncmp(url, "http://", 7) != 0) return -1;
    url += 7;

    char host[256], path[256];
    int i = 0, j = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) host[i++] = *url++;
    host[i] = '\0';

    uint16_t port = 80;
    if (*url == ':') {
        url++;
        port = 0;
        while (*url >= '0' && *url <= '9') { port = port * 10 + (*url - '0'); url++; }
    }

    if (*url == '/') { while (*url && j < 255) path[j++] = *url++; path[j] = '\0'; }
    else { path[0] = '/'; path[1] = '\0'; }

    uint32_t ip;
    int host_is_ip = 1;
    for (const char *p = host; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { host_is_ip = 0; break; }
    if (host_is_ip) {
        ip = 0;
        int shift = 0, val = 0;
        for (const char *p = host; *p; p++) {
            if (*p == '.') { ip |= (uint32_t)(val & 0xFF) << shift; shift += 8; val = 0; }
            else val = val * 10 + (*p - '0');
        }
        ip |= (uint32_t)(val & 0xFF) << shift;
    } else if (dns_resolve(host, &ip) != 0) {
        return -1;
    }

    static char resp[8192];
    int n = http_get(ip, host, port, path, (uint8_t *)resp, sizeof(resp) - 1);
    if (n <= 0) return -1;
    resp[n] = '\0';

    /* The response is the raw "status line + headers + \r\n\r\n + body"
     * HTTP/1.0 reply; callers want just the body, same as a browser. */
    const char *body = strstr(resp, "\r\n\r\n");
    body = body ? body + 4 : resp;

    int blen = n - (int)(body - resp);
    if (blen < 0) blen = 0;
    if (blen > out_max - 1) blen = out_max - 1;
    for (int k = 0; k < blen; k++) out[k] = body[k];
    out[blen] = 0;
    return blen;
}

/* ── Audio playback ─────────────────────────────────────────────────────── */

typedef uint32_t (*decode_read_fn)(void *ctx, uint8_t *out, uint32_t out_len);

/* Shared by every format: all three decoders' *_read() already produce
 * 8-bit unsigned mono 22050 Hz PCM, i.e. exactly what audio_submit()
 * wants, so one loop drives all of them via a common-shaped function
 * pointer (same calling convention regardless of the ctx struct type
 * each was actually declared to take). */
static void submit_pcm_loop(decode_read_fn read_fn, void *ctx)
{
    static uint8_t buf[AUDIO_DMA_SIZE];
    for (;;) {
        uint32_t n = read_fn(ctx, buf, AUDIO_DMA_SIZE);
        if (n == 0) break;
        uint32_t deadline = debugmon_uptime_ms() + 2000;
        while (audio_busy() && debugmon_uptime_ms() < deadline) { }
        audio_submit(buf, n);
    }
    uint32_t deadline = debugmon_uptime_ms() + 2000;
    while (audio_busy() && debugmon_uptime_ms() < deadline) { }
}

int tos_play_file(const char *path)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    uint32_t sz = fsbridge_size(path);
    if (!sz || sz > 32U * 1024U * 1024U) return -1;

    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return -1;
    fsbridge_read(path, buf, sz, 0);

    if (!audio_available()) audio_init();
    if (!audio_available()) { free(buf); return -1; }

    int rc = -1;

    if (sz >= 12 && buf[0] == 'R' && buf[1] == 'I' && buf[8] == 'W' && buf[9] == 'A') {
        wav_ctx_t wav;
        if (wav_open(&wav, buf, sz) == 0) {
            submit_pcm_loop((decode_read_fn)wav_read, &wav);
            rc = 0;
        }
    } else if (sz >= 4 &&
               ((buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0 && (buf[1] & 0x06) == 0x02) ||
                (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3'))) {
        uint32_t off = 0;
        if (buf[0] == 'I' && sz > 10)
            off = 10U + (((uint32_t)buf[6] & 0x7F) << 21 | ((uint32_t)buf[7] & 0x7F) << 14 |
                          ((uint32_t)buf[8] & 0x7F) << 7  | ((uint32_t)buf[9] & 0x7F));
        mp3_ctx_t mp3;
        if (off < sz && mp3_open(&mp3, buf + off, sz - off) == 0) {
            submit_pcm_loop((decode_read_fn)mp3_read, &mp3);
            rc = 0;
        }
    } else {
        /* Try as an M4A/MP4 container first (moov/mdat boxes wrapping
         * headerless AAC-LC samples); fall back to a bare ADTS stream
         * (a plain .aac file) if that fails. */
        m4a_result_t m4a;
        if (m4a_demux(buf, sz, &m4a) == 0) {
            aac_ctx_t aac;
            if (aac_open(&aac, m4a.adts_buf, m4a.adts_len) == 0) {
                submit_pcm_loop((decode_read_fn)aac_read, &aac);
                rc = 0;
            }
            m4a_free(&m4a);
        } else {
            aac_ctx_t aac;
            if (aac_open(&aac, buf, sz) == 0) {
                submit_pcm_loop((decode_read_fn)aac_read, &aac);
                rc = 0;
            }
        }
    }

    free(buf);
    return rc;
}

void tos_stop_audio(void) { audio_stop(); }
void tos_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    audio_set_volume((uint8_t)vol);
}
int tos_audio_playing(void) { return audio_busy(); }
