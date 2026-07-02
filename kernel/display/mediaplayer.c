#include "mediaplayer.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"
#include "scheduler.h"
#include "audio.h"
#include "wav_decoder.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "demo_song.h"
#include "vfs.h"
#include "stdio.h"

/* ── Layout (window content area assumed ≥ 60×18) ──────────────────────── */
#define MP_COLS   79
#define MP_ROWS   22

#define ROW_TOOLBAR  0
#define ROW_DIVIDER  1
#define ROW_SEEKBAR  2
#define ROW_INFO     3
#define ROW_STATUS   4
#define ROW_VOLBAR   5
#define ROW_DIV2     6
#define ROW_PLIST    7        /* playlist starts here */
#define PLIST_ROWS   14       /* rows available for playlist */
#define MAX_PLIST    64

/* VGA color shorthands */
#define C(fg,bg) ((uint8_t)((fg)|(((uint8_t)(bg))<<4)))
#define BLK VGA_BLACK
#define WHT VGA_WHITE
#define BLU VGA_BLUE
#define LBL VGA_LIGHT_BLUE
#define CYN VGA_CYAN
#define LGY VGA_LIGHT_GREY
#define DGY VGA_DARK_GREY
#define GRN VGA_GREEN
#define LGN VGA_LIGHT_GREEN
#define YLW VGA_BROWN
#define RED VGA_RED
#define LRD VGA_LIGHT_RED
#define MGN VGA_MAGENTA

/* ── State ──────────────────────────────────────────────────────────────── */
typedef enum { FMT_NONE=0, FMT_WAV, FMT_MP3, FMT_AAC } fmt_t;
typedef enum { ST_STOPPED=0, ST_PLAYING, ST_PAUSED  } state_t;

static state_t  g_state    = ST_STOPPED;
static fmt_t    g_fmt      = FMT_NONE;
static char     g_path[64] = {0};
static uint8_t *g_filebuf  = NULL;
static uint32_t g_filesz   = 0;

static wav_ctx_t g_wav;
static mp3_ctx_t g_mp3;
static aac_ctx_t g_aac;

/* DMA work buffer (one chunk at a time) */
static uint8_t  g_pcmbuf[AUDIO_DMA_SIZE];
static uint32_t g_pcmfill  = 0;
static uint32_t g_pcmpos   = 0;

static uint8_t  g_volume   = 80;   /* 0-100 */
static int      g_loop     = 0;
static uint32_t g_dur_sec  = 0;

/* Playlist */
static char     g_plist[MAX_PLIST][64];
static int      g_plist_n    = 0;
static int      g_plist_sel  = -1;  /* currently playing */
static int      g_plist_view = 0;   /* top visible item */

static char g_status[MP_COLS+1] = "tOS Media Player - ready";
static int  g_needs_redraw = 1;

static char g_pending_path[64] = {0};
static int  g_has_pending = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void mp_put(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void mp_fill(int x, int y, int w, char c, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    for (int i = 0; i < w; i++) terminal_putchar(c);
}

static void mp_fmt_time(char *buf, uint32_t sec)
{
    uint32_t m = sec / 60, s = sec % 60;
    buf[0] = (char)('0' + m/10); buf[1] = (char)('0' + m%10);
    buf[2] = ':';
    buf[3] = (char)('0' + s/10); buf[4] = (char)('0' + s%10);
    buf[5] = 0;
}

/* ── Audio file loading ─────────────────────────────────────────────────── */
static void mp_free_file(void)
{
    if (g_filebuf) { free(g_filebuf); g_filebuf = NULL; }
    g_filesz = 0;
    g_fmt    = FMT_NONE;
    g_state  = ST_STOPPED;
    g_pcmfill = 0;
    g_pcmpos  = 0;
}

static int mp_load(const char *path)
{
    mp_free_file();
    audio_stop();

    /* built-in demo song? */
    int is_demo = (strcmp(path, "/demo/demo.wav") == 0 ||
                   strcmp(path, "demo.wav") == 0);

    if (is_demo) {
        /* use embedded PCM directly */
        if (wav_open(&g_wav, NULL, 0) == 0) { /* no-op */ }
        /* pretend WAV with demo_song_pcm */
        g_fmt  = FMT_WAV;
        g_filebuf = NULL;
        /* We'll directly feed demo_song_pcm in mp_fill_buf */
        g_filesz  = DEMO_SONG_LEN;
        g_dur_sec = DEMO_SONG_LEN / DEMO_SONG_RATE;
        g_pcmfill = 0; g_pcmpos = 0;
        strncpy(g_path, path, 63);
        return 0;
    }

    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) {
        strncpy(g_status, "Error: file not found.", MP_COLS);
        return -1;
    }
    uint32_t sz = fsbridge_size(path);
    if (!sz || sz > 32*1024*1024U) {
        strncpy(g_status, "Error: file too large (>32MB).", MP_COLS);
        return -1;
    }
    g_filebuf = (uint8_t*)malloc(sz);
    if (!g_filebuf) {
        strncpy(g_status, "Error: out of memory.", MP_COLS);
        return -1;
    }
    fsbridge_read(path, g_filebuf, sz, 0);
    g_filesz = sz;
    strncpy(g_path, path, 63);

    /* detect format */
    if (sz >= 12 && g_filebuf[0]=='R' && g_filebuf[1]=='I' &&
        g_filebuf[8]=='W' && g_filebuf[9]=='A') {
        if (wav_open(&g_wav, g_filebuf, sz) == 0) {
            g_fmt = FMT_WAV;
            g_dur_sec = wav_duration_sec(&g_wav);
        }
    } else if (sz >= 4 &&
               ((g_filebuf[0]==0xFF && (g_filebuf[1]&0xE0)==0xE0 && (g_filebuf[1]&0x06)==0x02) ||
                (g_filebuf[0]=='I' && g_filebuf[1]=='D' && g_filebuf[2]=='3'))) {
        /* MP3 or ID3+MP3 */
        uint32_t mp3off = 0;
        if (g_filebuf[0]=='I') {
            /* skip ID3v2 tag */
            if (sz > 10)
                mp3off = 10U + (uint32_t)(((g_filebuf[6]&0x7F)<<21) |
                                           ((g_filebuf[7]&0x7F)<<14) |
                                           ((g_filebuf[8]&0x7F)<<7)  |
                                            (g_filebuf[9]&0x7F));
        }
        if (mp3_open(&g_mp3, g_filebuf+mp3off, sz-mp3off) == 0) {
            g_fmt = FMT_MP3;
            g_dur_sec = mp3_duration_sec(&g_mp3);
        }
    } else {
        int rc = aac_open(&g_aac, g_filebuf, sz);
        if (rc == -2) {
            g_fmt = FMT_AAC;
            strncpy(g_status, "AAC: format detected - decoder coming soon.", MP_COLS);
            return -1;
        }
        strncpy(g_status, "Unknown format (WAV/MP3/AAC supported).", MP_COLS);
        mp_free_file();
        return -1;
    }

    g_pcmfill = 0;
    g_pcmpos  = 0;
    return 0;
}

/* Fill g_pcmbuf with up to AUDIO_DMA_SIZE bytes of decoded 8-bit PCM */
static void mp_fill_pcm(void)
{
    uint32_t n = 0;
    /* Demo song: serve directly from embedded array */
    int is_demo = (g_filebuf == NULL && g_fmt == FMT_WAV);
    if (is_demo) {
        while (n < AUDIO_DMA_SIZE && g_pcmpos < DEMO_SONG_LEN) {
            g_pcmbuf[n++] = demo_song_pcm[g_pcmpos++];
        }
        if (!n && g_loop) g_pcmpos = 0;
    } else if (g_fmt == FMT_WAV) {
        n = wav_read(&g_wav, g_pcmbuf, AUDIO_DMA_SIZE);
        if (!n && g_loop) {
            wav_seek(&g_wav, 0);
            n = wav_read(&g_wav, g_pcmbuf, AUDIO_DMA_SIZE);
        }
    } else if (g_fmt == FMT_MP3) {
        n = mp3_read(&g_mp3, g_pcmbuf, AUDIO_DMA_SIZE);
        if (!n && g_loop) {
            mp3_seek_sec(&g_mp3, 0);
            n = mp3_read(&g_mp3, g_pcmbuf, AUDIO_DMA_SIZE);
        }
    }
    g_pcmfill = n;
}

/* ── Playlist management ──────────────────────────────────────────────────── */
static int is_audio_ext(const char *name)
{
    int n = (int)strlen(name);
    if (n > 4 && (name[n-4]=='.'||name[n-3]=='.')) {
        const char *ext = name + n - 4;
        if (ext[0]=='.') ext++;
        if ((ext[0]=='w'||ext[0]=='W') && (ext[1]=='a'||ext[1]=='A') &&
            (ext[2]=='v'||ext[2]=='V')) return 1;
        if ((ext[0]=='m'||ext[0]=='M') && (ext[1]=='p'||ext[1]=='P') &&
            (ext[2]=='3')) return 1;
        if ((ext[0]=='m'||ext[0]=='M') && (ext[1]=='4'||ext[1]=='4') &&
            (ext[2]=='a'||ext[2]=='A')) return 1;
        if ((ext[0]=='a'||ext[0]=='A') && (ext[1]=='a'||ext[1]=='A') &&
            (ext[2]=='c'||ext[2]=='C')) return 1;
    }
    return 0;
}

static void playlist_scan(const char *dir)
{
    vfs_entry_t entries[64];
    int n = fsbridge_list(dir, entries, 64);
    for (int i = 0; i < n && g_plist_n < MAX_PLIST; i++) {
        if (entries[i].is_dir) continue;
        if (!is_audio_ext(entries[i].name)) continue;
        int dl = (int)strlen(dir);
        int nl = (int)strlen(entries[i].name);
        if (dl + nl + 2 < 64) {
            char *p = g_plist[g_plist_n];
            int j = 0;
            for (int k = 0; k < dl; k++) p[j++] = dir[k];
            if (dir[dl-1] != '/') p[j++] = '/';
            for (int k = 0; k < nl; k++) p[j++] = entries[i].name[k];
            p[j] = 0;
            g_plist_n++;
        }
    }
}

static void playlist_add(const char *path)
{
    if (g_plist_n >= MAX_PLIST) return;
    strncpy(g_plist[g_plist_n], path, 63);
    g_plist_n++;
}

static void playlist_play_idx(int idx)
{
    if (idx < 0 || idx >= g_plist_n) return;
    g_plist_sel = idx;
    audio_stop();
    if (mp_load(g_plist[idx]) == 0) {
        g_state = ST_PLAYING;
        /* find basename for status */
        const char *p = g_plist[idx];
        const char *last = p;
        while (*p) { if (*p=='/') last=p+1; p++; }
        snprintf(g_status, MP_COLS, "Playing: %s", last);
    } else {
        g_state = ST_STOPPED;
    }
}

/* ── Drawing ──────────────────────────────────────────────────────────────── */
static void draw_toolbar(void)
{
    mp_fill(0, ROW_TOOLBAR, MP_COLS, ' ', C(BLK, LGY));

    /* Play/Pause/Stop */
    const char *play_lbl = (g_state == ST_PLAYING) ? "[II Pause]" : "[ > Play ]";
    mp_put(1, ROW_TOOLBAR, play_lbl, C(BLK, BLU));
    mp_put(12, ROW_TOOLBAR, "[-- Stop]", C(WHT, RED));
    mp_put(22, ROW_TOOLBAR, "[<< Prev]", C(BLK, LGY));
    mp_put(31, ROW_TOOLBAR, "[>> Next]", C(BLK, LGY));

    /* Loop toggle */
    const char *loop_lbl = g_loop ? "[LOOP:ON ]" : "[LOOP:OFF]";
    mp_put(41, ROW_TOOLBAR, loop_lbl, C(BLK, g_loop ? GRN : DGY));

    /* Audio status */
    if (!audio_available())
        mp_put(53, ROW_TOOLBAR, "[No Audio HW]", C(YLW, DGY));
    else
        mp_put(53, ROW_TOOLBAR, "[Audio OK]    ", C(LGN, DGY));
}

static void draw_divider(int row, const char *label)
{
    mp_fill(0, row, MP_COLS, '\xC4', C(DGY, BLK));
    if (label) {
        int llen = (int)strlen(label);
        int lx   = (MP_COLS - llen - 2) / 2;
        mp_put(lx, row, " ", C(DGY, BLK));
        mp_put(lx+1, row, label, C(CYN, BLK));
        mp_put(lx+1+llen, row, " ", C(DGY, BLK));
    }
}

static void draw_seekbar(void)
{
    uint32_t pos_s = 0;
    if (g_fmt == FMT_WAV && g_filebuf == NULL) {
        /* demo: position from pcmpos */
        pos_s = g_pcmpos / DEMO_SONG_RATE;
    } else if (g_fmt == FMT_WAV) {
        pos_s = wav_pos_sec(&g_wav);
    } else if (g_fmt == FMT_MP3) {
        pos_s = mp3_pos_sec(&g_mp3);
    }

    char tpos[8], tdur[8];
    mp_fmt_time(tpos, pos_s);
    mp_fmt_time(tdur, g_dur_sec ? g_dur_sec : 0);

    /* Seek bar: [====|         ] POS / DUR */
    int bar_w = 48;
    int filled = (g_dur_sec > 0 && pos_s <= g_dur_sec)
                 ? (int)((uint64_t)pos_s * bar_w / g_dur_sec)
                 : 0;

    mp_put(0, ROW_SEEKBAR, "[", C(LGY, BLK));
    for (int i = 0; i < bar_w; i++) {
        if (i < filled)
            terminal_putchar('\xDB'); /* filled block */
        else if (i == filled)
            terminal_putchar('|');   /* cursor */
        else
            terminal_putchar('\xB0'); /* light shade */
    }
    mp_put(bar_w+1, ROW_SEEKBAR, "] ", C(LGY, BLK));
    mp_put(bar_w+3, ROW_SEEKBAR, tpos, C(WHT, BLK));
    mp_put(bar_w+8, ROW_SEEKBAR, " / ", C(DGY, BLK));
    mp_put(bar_w+11, ROW_SEEKBAR, tdur, C(LGY, BLK));
}

static void draw_info(void)
{
    mp_fill(0, ROW_INFO, MP_COLS, ' ', C(LGY, BLK));

    /* Track name */
    const char *name = "(no file)";
    const char *fmt_str = "";
    if (g_path[0]) {
        const char *p = g_path;
        const char *last = p;
        while (*p) { if (*p=='/') last=p+1; p++; }
        name = last;
        fmt_str = (g_fmt==FMT_WAV)?"[WAV]":
                  (g_fmt==FMT_MP3)?"[MP3]":
                  (g_fmt==FMT_AAC)?"[AAC]":"[???]";
    }

    char buf[MP_COLS+1];
    int n = 0;
    while (name[n] && n < 44) { buf[n] = name[n]; n++; }
    buf[n] = 0;
    mp_put(0, ROW_INFO, fmt_str, C(LBL, BLK));
    mp_put(6, ROW_INFO, buf, C(WHT, BLK));
}

static void draw_status(void)
{
    mp_fill(0, ROW_STATUS, MP_COLS, ' ', C(DGY, BLK));
    const char *state_str = (g_state==ST_PLAYING) ? ">> PLAYING" :
                            (g_state==ST_PAUSED)  ? "|| PAUSED " :
                                                    "[] STOPPED";
    uint8_t sc = (g_state==ST_PLAYING) ? C(LGN,BLK) :
                 (g_state==ST_PAUSED)  ? C(YLW,BLK) : C(DGY,BLK);
    mp_put(0, ROW_STATUS, state_str, sc);
    mp_put(11, ROW_STATUS, "  ", C(DGY,BLK));
    mp_put(13, ROW_STATUS, g_status, C(LGY, BLK));
}

static void draw_volbar(void)
{
    mp_fill(0, ROW_VOLBAR, MP_COLS, ' ', C(DGY, BLK));
    mp_put(0, ROW_VOLBAR, "Vol: [", C(DGY, BLK));
    int vw = (int)(g_volume * 20 / 100);
    for (int i = 0; i < 20; i++) {
        uint8_t c2 = (i < vw) ? C(LGN,BLK) : C(DGY,BLK);
        terminal_setcolor(c2);
        terminal_putchar(i < vw ? '\xDB' : '\xB0');
    }
    char vbuf[8];
    vbuf[0] = ']'; vbuf[1] = ' ';
    vbuf[2] = '0' + g_volume/100;
    vbuf[3] = '0' + (g_volume%100)/10;
    vbuf[4] = '0' + g_volume%10;
    vbuf[5] = '%'; vbuf[6] = 0;
    mp_put(26, ROW_VOLBAR, vbuf, C(WHT,BLK));
    mp_put(34, ROW_VOLBAR, "  [+] Vol+  [-] Vol-  [Enter] Pause  [Esc] Stop  [<>] Seek", C(DGY,BLK));
}

static void draw_playlist(void)
{
    draw_divider(ROW_DIV2, "Playlist");
    for (int r = 0; r < PLIST_ROWS; r++) {
        int idx = g_plist_view + r;
        if (idx >= g_plist_n) {
            mp_fill(0, ROW_PLIST + r, MP_COLS, ' ', C(DGY, BLK));
            continue;
        }
        /* basename */
        const char *p = g_plist[idx];
        const char *last = p;
        while (*p) { if (*p=='/') last=p+1; p++; }

        uint8_t col;
        if (idx == g_plist_sel)
            col = C(BLK, LGN);     /* currently playing */
        else if (idx == g_plist_view + r)
            col = C(WHT, BLK);     /* normal */
        else
            col = C(LGY, BLK);

        char line[MP_COLS+1];
        int li = 0;
        /* index number */
        line[li++] = (char)(' ');
        line[li++] = (char)('0' + (idx+1)/10);
        line[li++] = (char)('0' + (idx+1)%10);
        line[li++] = '.';
        line[li++] = ' ';
        while (*last && li < MP_COLS-1) line[li++] = *last++;
        while (li < MP_COLS) line[li++] = ' ';
        line[MP_COLS] = 0;
        mp_put(0, ROW_PLIST + r, line, col);
    }
}

static void redraw(void)
{
    draw_toolbar();
    draw_divider(ROW_DIVIDER, g_path[0] ? g_path : "tOS Media Player v0.9.43");
    draw_seekbar();
    draw_info();
    draw_status();
    draw_volbar();
    draw_playlist();
    g_needs_redraw = 0;
}

/* ── Button hit testing ───────────────────────────────────────────────────── */
static void handle_toolbar_click(int cx, int cy)
{
    (void)cy;
    if (cx >= 1  && cx <= 10) {
        /* Play / Pause */
        if (g_state == ST_PLAYING) {
            audio_stop(); g_state = ST_PAUSED;
        } else if (g_state == ST_PAUSED) {
            g_state = ST_PLAYING;
        } else {
            if (g_plist_n > 0) {
                int idx = (g_plist_sel >= 0) ? g_plist_sel : 0;
                playlist_play_idx(idx);
            }
        }
    } else if (cx >= 12 && cx <= 20) {
        /* Stop */
        audio_stop(); g_state = ST_STOPPED; mp_free_file();
        strncpy(g_status, "Stopped.", MP_COLS);
    } else if (cx >= 22 && cx <= 30) {
        /* Prev */
        if (g_plist_sel > 0) playlist_play_idx(g_plist_sel - 1);
    } else if (cx >= 31 && cx <= 39) {
        /* Next */
        if (g_plist_sel < g_plist_n - 1) playlist_play_idx(g_plist_sel + 1);
    } else if (cx >= 41 && cx <= 50) {
        g_loop = !g_loop;
    }
    g_needs_redraw = 1;
}

static void handle_seekbar_click(int cx)
{
    /* bar spans columns 1..48 */
    if (cx < 1 || cx > 48 || !g_dur_sec) return;
    uint32_t target = (uint32_t)((uint64_t)(cx-1) * g_dur_sec / 47);
    if (g_fmt == FMT_WAV && g_filebuf) wav_seek(&g_wav, target * AUDIO_OUT_RATE);
    else if (g_fmt == FMT_MP3) mp3_seek_sec(&g_mp3, target);
    else if (g_fmt == FMT_WAV && !g_filebuf) {
        g_pcmpos = target * DEMO_SONG_RATE;
        if (g_pcmpos > DEMO_SONG_LEN) g_pcmpos = DEMO_SONG_LEN;
    }
    g_pcmfill = 0;
    g_needs_redraw = 1;
}

static void handle_plist_click(int row_in_plist)
{
    int idx = g_plist_view + row_in_plist;
    if (idx < g_plist_n) playlist_play_idx(idx);
    g_needs_redraw = 1;
}

/* ── Keyboard handler ────────────────────────────────────────────────────── */
static void handle_key(char k) __attribute__((unused));
static void handle_key(char k)
{
    switch (k) {
    case '\r': case '\n':
        /* Enter = pause/resume */
        if (g_state == ST_PLAYING) {
            audio_stop(); g_state = ST_PAUSED;
        } else if (g_state == ST_PAUSED) {
            g_state = ST_PLAYING;
        }
        break;
    case 27: /* Esc = stop */
        audio_stop(); g_state = ST_STOPPED;
        break;
    case '+': case '=':
        if (g_volume < 100) { g_volume += 5; if(g_volume>100)g_volume=100; }
        audio_set_volume(g_volume);
        break;
    case '-':
        if (g_volume > 0) { if(g_volume<5) g_volume=0; else g_volume-=5; }
        audio_set_volume(g_volume);
        break;
    case 'l': case 'L':
        g_loop = !g_loop;
        break;
    case 'n': case 'N':
        if (g_plist_sel < g_plist_n-1) playlist_play_idx(g_plist_sel+1);
        break;
    case 'p': case 'P':
        if (g_plist_sel > 0) playlist_play_idx(g_plist_sel-1);
        break;
    }
    g_needs_redraw = 1;
}

static void handle_special_key(int spec)
{
    switch (spec) {
    case 1: /* left = seek back 5s */
        if (g_fmt == FMT_WAV && g_filebuf) {
            uint32_t p = wav_pos_sec(&g_wav);
            wav_seek(&g_wav, (p>5?(p-5):0) * AUDIO_OUT_RATE);
            g_pcmfill = 0;
        } else if (g_fmt == FMT_MP3) {
            uint32_t p = mp3_pos_sec(&g_mp3);
            mp3_seek_sec(&g_mp3, p>5?p-5:0);
            g_pcmfill = 0;
        } else if (g_fmt == FMT_WAV && !g_filebuf) {
            if (g_pcmpos > 5*DEMO_SONG_RATE) g_pcmpos -= 5*DEMO_SONG_RATE;
            else g_pcmpos = 0;
            g_pcmfill = 0;
        }
        break;
    case 2: /* right = seek forward 5s */
        if (g_fmt == FMT_WAV && g_filebuf) {
            uint32_t p = wav_pos_sec(&g_wav);
            wav_seek(&g_wav, (p+5) * AUDIO_OUT_RATE);
            g_pcmfill = 0;
        } else if (g_fmt == FMT_MP3) {
            uint32_t p = mp3_pos_sec(&g_mp3);
            mp3_seek_sec(&g_mp3, p+5);
            g_pcmfill = 0;
        } else if (g_fmt == FMT_WAV && !g_filebuf) {
            g_pcmpos += 5*DEMO_SONG_RATE;
            if (g_pcmpos > DEMO_SONG_LEN) g_pcmpos = DEMO_SONG_LEN;
            g_pcmfill = 0;
        }
        break;
    case 3: /* up = scroll plist up */
        if (g_plist_view > 0) g_plist_view--;
        break;
    case 4: /* down = scroll plist down */
        if (g_plist_view + PLIST_ROWS < g_plist_n) g_plist_view++;
        break;
    }
    g_needs_redraw = 1;
}

/* ── Main entry point ────────────────────────────────────────────────────── */
void mediaplayer_open_path(const char *path)
{
    strncpy(g_pending_path, path, 63);
    g_has_pending = 1;
}

void mediaplayer_run(void)
{
    /* reset state */
    g_state    = ST_STOPPED;
    g_fmt      = FMT_NONE;
    g_filebuf  = NULL;
    g_filesz   = 0;
    g_volume   = 80;
    g_loop     = 0;
    g_plist_n  = 0;
    g_plist_sel= -1;
    g_plist_view=0;
    g_pcmfill  = 0;
    g_pcmpos   = 0;
    g_path[0]  = 0;
    g_needs_redraw = 1;
    strncpy(g_status, "tOS Media Player - ready", MP_COLS);

    audio_init();
    audio_set_volume(g_volume);

    /* Add demo song to playlist */
    playlist_add("/demo/demo.wav");
    /* Scan /music and / for audio files */
    playlist_scan("/music");
    playlist_scan("/");

    /* Handle pending file or auto-start demo */
    if (g_has_pending) {
        g_has_pending = 0;
        playlist_add(g_pending_path);
        playlist_play_idx(g_plist_n - 1);
    } else {
        /* auto-play demo song */
        playlist_play_idx(0);
    }

    terminal_clear();
    redraw();

    for (;;) {
        gui_poll();
        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        /* ── Audio pump: submit next PCM chunk when hardware is free ──── */
        if (g_state == ST_PLAYING && audio_available() && !audio_busy()) {
            if (g_pcmfill == 0) {
                mp_fill_pcm();
                if (g_pcmfill == 0) {
                    /* EOF */
                    if (g_plist_sel >= 0 && g_plist_sel < g_plist_n-1) {
                        playlist_play_idx(g_plist_sel + 1);
                    } else if (g_loop) {
                        playlist_play_idx(g_plist_sel);
                    } else {
                        g_state = ST_STOPPED;
                        strncpy(g_status, "Playback complete.", MP_COLS);
                    }
                    g_needs_redraw = 1;
                }
            }
            if (g_pcmfill > 0) {
                audio_submit(g_pcmbuf, g_pcmfill);
                g_pcmfill = 0;
                g_needs_redraw = 1; /* update seek bar */
            }
        }

        /* ── Mouse / click handling ───────────────────────────────────── */
        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == ROW_TOOLBAR)
                handle_toolbar_click(ccx, ccy);
            else if (ccy == ROW_SEEKBAR)
                handle_seekbar_click(ccx);
            else if (ccy >= ROW_PLIST && ccy < ROW_PLIST + PLIST_ROWS)
                handle_plist_click(ccy - ROW_PLIST);
        }

        /* ── Keyboard ────────────────────────────────────────────────── */
        int spec = keyboard_get_special();
        if (spec) handle_special_key(spec);

        /* keyboard_getchar() blocks; we skip it here — mouse clicks handle all UI */

        /* ── Redraw (every cycle during playback for seek bar update) ── */
        if (g_needs_redraw || g_state == ST_PLAYING) {
            redraw();
        }

        task_yield();
    }
}
