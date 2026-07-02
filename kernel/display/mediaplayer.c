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
#include "version.h"

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define MP_COLS   79
#define MP_ROWS   22

/*
   Row layout:
   0  ┌─ Toolbar: [►Play] [■Stop] [|◄Prev] [Next►|] [LOOP] [Audio]
   1  ├─ ══ filename ══
   2  │  Seek bar [████|░░░░] 01:23 / 03:45   [◄◄5s] [5s►►]
   3  │  [WAV] filename.wav
   4  │  >> PLAYING   status message
   5  │  Vol: [▓▓▓▓░░] 080%  [-] [+]
   6  ├─ ══ Playlist ══
   7-20   playlist items (clickable)
*/
#define ROW_TOOLBAR  0
#define ROW_DIVIDER  1
#define ROW_SEEKBAR  2
#define ROW_INFO     3
#define ROW_STATUS   4
#define ROW_VOLBAR   5
#define ROW_DIV2     6
#define ROW_PLIST    7
#define PLIST_ROWS   14
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

/* ── State ───────────────────────────────────────────────────────────────── */
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

static uint8_t  g_pcmbuf[AUDIO_DMA_SIZE];
static uint32_t g_pcmfill  = 0;
static uint32_t g_pcmpos   = 0;

static uint8_t  g_volume   = 80;
static int      g_loop     = 0;
static uint32_t g_dur_sec  = 0;

static char     g_plist[MAX_PLIST][64];
static int      g_plist_n    = 0;
static int      g_plist_sel  = -1;
static int      g_plist_view = 0;

static char g_status[MP_COLS+1] = "tOS Media Player";

static char g_pending_path[64] = {0};
static int  g_has_pending = 0;

/* Dirty flags */
#define DIRTY_TOOLBAR  (1<<0)
#define DIRTY_DIVIDER  (1<<1)
#define DIRTY_SEEKBAR  (1<<2)
#define DIRTY_INFO     (1<<3)
#define DIRTY_STATUS   (1<<4)
#define DIRTY_VOLBAR   (1<<5)
#define DIRTY_PLIST    (1<<6)
#define DIRTY_ALL      0x7F

static int      g_dirty        = DIRTY_ALL;
static uint32_t g_last_pos_sec = 0xFFFFFFFFU;

/* SW timer: counts loop iterations. task_yield() ~10ms → ~100 iter/sec.
   Use 80 so position advances a bit faster than real time (safe margin). */
#define SW_TICKS_PER_SEC  80
static uint32_t g_sw_tick    = 0;
static uint32_t g_sw_pos_sec = 0;  /* software position counter (always used) */

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

static uint32_t current_pos_sec(void)
{
    /* SW timer always advances even if HW audio is broken */
    return g_sw_pos_sec;
}

/* ── Audio file loading ──────────────────────────────────────────────────── */
static void mp_free_file(void)
{
    if (g_filebuf) { free(g_filebuf); g_filebuf = NULL; }
    g_filesz = 0; g_fmt = FMT_NONE; g_state = ST_STOPPED;
    g_pcmfill = 0; g_pcmpos = 0; g_dur_sec = 0;
    g_sw_pos_sec = 0; g_sw_tick = 0;
    g_last_pos_sec = 0xFFFFFFFFU;
}

static int mp_load(const char *path)
{
    mp_free_file();
    audio_stop();

    if (strcmp(path, "/demo/demo.wav")==0 || strcmp(path,"demo.wav")==0) {
        g_fmt = FMT_WAV; g_filebuf = NULL;
        g_filesz = DEMO_SONG_LEN;
        g_dur_sec = DEMO_SONG_LEN / DEMO_SONG_RATE;
        strncpy(g_path, path, 63);
        return 0;
    }

    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) {
        strncpy(g_status, "Error: file not found.", MP_COLS); return -1;
    }
    uint32_t sz = fsbridge_size(path);
    if (!sz || sz > 32*1024*1024U) {
        strncpy(g_status, "Error: file too large.", MP_COLS); return -1;
    }
    g_filebuf = (uint8_t*)malloc(sz);
    if (!g_filebuf) { strncpy(g_status, "Error: out of memory.", MP_COLS); return -1; }
    fsbridge_read(path, g_filebuf, sz, 0);
    g_filesz = sz;
    strncpy(g_path, path, 63);

    if (sz>=12 && g_filebuf[0]=='R' && g_filebuf[1]=='I' &&
        g_filebuf[8]=='W' && g_filebuf[9]=='A') {
        if (wav_open(&g_wav, g_filebuf, sz)==0) {
            g_fmt = FMT_WAV; g_dur_sec = wav_duration_sec(&g_wav);
        }
    } else if (sz>=4 &&
        ((g_filebuf[0]==0xFF && (g_filebuf[1]&0xE0)==0xE0 && (g_filebuf[1]&0x06)==0x02) ||
         (g_filebuf[0]=='I' && g_filebuf[1]=='D' && g_filebuf[2]=='3'))) {
        uint32_t off = 0;
        if (g_filebuf[0]=='I' && sz>10)
            off = 10U + (uint32_t)((((uint32_t)g_filebuf[6]&0x7F)<<21)|
                                    (((uint32_t)g_filebuf[7]&0x7F)<<14)|
                                    (((uint32_t)g_filebuf[8]&0x7F)<<7) |
                                     ((uint32_t)g_filebuf[9]&0x7F));
        if (mp3_open(&g_mp3, g_filebuf+off, sz-off)==0) {
            g_fmt = FMT_MP3; g_dur_sec = mp3_duration_sec(&g_mp3);
        }
    } else {
        if (aac_open(&g_aac, g_filebuf, sz)==0) {
            g_fmt = FMT_AAC; g_dur_sec = aac_duration_sec(&g_aac);
        } else { strncpy(g_status,"Unknown format.",MP_COLS); mp_free_file(); return -1; }
    }
    return 0;
}

static void mp_fill_pcm(void)
{
    uint32_t n = 0;
    if (g_fmt==FMT_WAV && !g_filebuf) {
        while (n < AUDIO_DMA_SIZE && g_pcmpos < DEMO_SONG_LEN)
            g_pcmbuf[n++] = demo_song_pcm[g_pcmpos++];
        if (!n && g_loop) g_pcmpos = 0;
    } else if (g_fmt==FMT_WAV && g_filebuf) {
        n = wav_read(&g_wav, g_pcmbuf, AUDIO_DMA_SIZE);
        if (!n && g_loop) { wav_seek(&g_wav,0); n=wav_read(&g_wav,g_pcmbuf,AUDIO_DMA_SIZE); }
    } else if (g_fmt==FMT_MP3) {
        n = mp3_read(&g_mp3, g_pcmbuf, AUDIO_DMA_SIZE);
        if (!n && g_loop) { mp3_seek_sec(&g_mp3,0); n=mp3_read(&g_mp3,g_pcmbuf,AUDIO_DMA_SIZE); }
    } else if (g_fmt==FMT_AAC) {
        n = aac_read(&g_aac, g_pcmbuf, AUDIO_DMA_SIZE);
        if (!n && g_loop) {
            aac_open(&g_aac, g_aac.data, g_aac.size);
            n = aac_read(&g_aac, g_pcmbuf, AUDIO_DMA_SIZE);
        }
    }
    g_pcmfill = n;
}

/* ── Playlist ────────────────────────────────────────────────────────────── */
static int is_audio_ext(const char *name)
{
    int n = (int)strlen(name);
    if (n > 4) {
        const char *e = name + n - 4;
        if (e[0]=='.') {
            if ((e[1]|32)=='w'&&(e[2]|32)=='a'&&(e[3]|32)=='v') return 1;
            if ((e[1]|32)=='m'&&(e[2]|32)=='p'&&e[3]=='3') return 1;
            if ((e[1]|32)=='m'&&e[2]=='4'&&(e[3]|32)=='a') return 1;
            if ((e[1]|32)=='a'&&(e[2]|32)=='a'&&(e[3]|32)=='c') return 1;
        }
    }
    return 0;
}

static void playlist_scan(const char *dir)
{
    vfs_entry_t entries[64];
    int n = fsbridge_list(dir, entries, 64);
    for (int i = 0; i < n && g_plist_n < MAX_PLIST; i++) {
        if (entries[i].is_dir || !is_audio_ext(entries[i].name)) continue;
        int dl = (int)strlen(dir), nl = (int)strlen(entries[i].name);
        if (dl+nl+2 < 64) {
            char *p = g_plist[g_plist_n]; int j=0;
            for (int k=0;k<dl;k++) p[j++]=dir[k];
            if (dir[dl-1]!='/') p[j++]='/';
            for (int k=0;k<nl;k++) p[j++]=entries[i].name[k];
            p[j]=0; g_plist_n++;
        }
    }
}

static void playlist_add(const char *path)
{
    if (g_plist_n < MAX_PLIST)
        strncpy(g_plist[g_plist_n++], path, 63);
}

static void playlist_play_idx(int idx)
{
    if (idx < 0 || idx >= g_plist_n) return;
    g_plist_sel = idx;
    if (g_plist_sel < g_plist_view) g_plist_view = g_plist_sel;
    if (g_plist_sel >= g_plist_view+PLIST_ROWS) g_plist_view = g_plist_sel-PLIST_ROWS+1;
    audio_stop();
    g_sw_tick = 0;
    if (mp_load(g_plist[idx])==0) {
        g_state = ST_PLAYING;
        const char *p=g_plist[idx], *last=p;
        while (*p) { if (*p=='/') last=p+1; p++; }
        snprintf(g_status, MP_COLS, "Playing: %s", last);
    } else {
        g_state = ST_STOPPED;
    }
    g_dirty = DIRTY_ALL;
}

static void do_seek_delta(int ds)
{
    if (g_fmt==FMT_WAV && g_filebuf) {
        uint32_t p=wav_pos_sec(&g_wav);
        uint32_t t=(ds<0&&(uint32_t)(-ds)>p)?0:p+(uint32_t)ds;
        wav_seek(&g_wav, t*AUDIO_OUT_RATE);
    } else if (g_fmt==FMT_MP3) {
        uint32_t p=mp3_pos_sec(&g_mp3);
        uint32_t t=(ds<0&&(uint32_t)(-ds)>p)?0:p+(uint32_t)ds;
        mp3_seek_sec(&g_mp3, t);
    } else if (g_fmt==FMT_WAV && !g_filebuf) {
        if (ds<0) { uint32_t d=(uint32_t)(-ds)*DEMO_SONG_RATE;
                    g_pcmpos=(g_pcmpos>d)?g_pcmpos-d:0; }
        else      { g_pcmpos+=(uint32_t)ds*DEMO_SONG_RATE;
                    if (g_pcmpos>DEMO_SONG_LEN) g_pcmpos=DEMO_SONG_LEN; }
    }
    g_pcmfill=0; g_dirty|=DIRTY_SEEKBAR;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/*
  Toolbar button map (columns, inclusive):
   1-10   [►Play  ] / [||Pause]
   12-19  [■ Stop ]
   21-29  [|◄ Prev]
   31-39  [Next ►|]
   41-50  [LOOP:OFF] / [LOOP:ON ]
   52-65  [Audio OK] / [No Audio]
*/
static void draw_toolbar(void)
{
    mp_fill(0, ROW_TOOLBAR, MP_COLS, ' ', C(BLK,LGY));

    const char *play_lbl = (g_state==ST_PLAYING) ? "[|| Pause]" : "[ ► Play ]";
    uint8_t play_col = (g_state==ST_PAUSED) ? C(YLW,BLU) : C(WHT,BLU);
    mp_put(1,  ROW_TOOLBAR, play_lbl, play_col);
    mp_put(12, ROW_TOOLBAR, "[■ Stop ]", C(WHT,RED));
    mp_put(22, ROW_TOOLBAR, "[|◄ Prev]", C(BLK,LGY));
    mp_put(32, ROW_TOOLBAR, "[Next ►|]", C(BLK,LGY));

    const char *lp = g_loop ? "[LOOP: ON]" : "[LOOP:OFF]";
    mp_put(42, ROW_TOOLBAR, lp, C(BLK, g_loop?GRN:DGY));

    if (!audio_available()) {
        mp_put(53, ROW_TOOLBAR, "[No Audio HW]", C(YLW,DGY));
    } else {
        char abuf[16] = "[";
        const char *bn = audio_backend_name();
        int i = 1;
        while (*bn && i < 13) abuf[i++] = *bn++;
        abuf[i++] = ']'; abuf[i] = 0;
        while (i < 14) { abuf[i++] = ' '; abuf[i] = 0; }
        mp_put(53, ROW_TOOLBAR, abuf, C(LGN,DGY));
    }
}

static void draw_divider(int row, const char *label)
{
    mp_fill(0, row, MP_COLS, '\xC4', C(DGY,BLK));
    if (label) {
        int llen=(int)strlen(label), lx=(MP_COLS-llen-2)/2;
        mp_put(lx,      row, " ",   C(DGY,BLK));
        mp_put(lx+1,    row, label, C(CYN,BLK));
        mp_put(lx+1+llen,row," ",   C(DGY,BLK));
    }
}

/*
  Seek bar row:
   0      '['
   1-48   bar
   49     ']'
   51-55  mm:ss
   56-58  ' / '
   59-63  mm:ss
   65-71  [◄◄ 5s]
   73-79  [5s ►►]
*/
static void draw_seekbar(void)
{
    uint32_t pos = current_pos_sec();
    char tpos[8], tdur[8];
    mp_fmt_time(tpos, pos);
    mp_fmt_time(tdur, g_dur_sec);

    int bar_w = 48;
    int filled = (g_dur_sec>0 && pos<=g_dur_sec)
                 ? (int)((uint64_t)pos*(uint32_t)bar_w/g_dur_sec) : 0;

    mp_put(0, ROW_SEEKBAR, "[", C(LGY,BLK));
    terminal_setpos(1, ROW_SEEKBAR);
    for (int i=0; i<bar_w; i++) {
        terminal_setcolor((i<filled)?C(LGN,BLK):C(DGY,BLK));
        if (i<filled)       terminal_putchar('\xDB');
        else if (i==filled) terminal_putchar('|');
        else                terminal_putchar('\xB0');
    }
    mp_put(bar_w+1, ROW_SEEKBAR, "] ", C(LGY,BLK));
    mp_put(bar_w+3, ROW_SEEKBAR, tpos,  C(WHT,BLK));
    mp_put(bar_w+8, ROW_SEEKBAR, " / ", C(DGY,BLK));
    mp_put(bar_w+11,ROW_SEEKBAR, tdur,  C(LGY,BLK));
    mp_put(65, ROW_SEEKBAR, "[◄◄ 5s]", C(BLK,LGY));
    mp_put(73, ROW_SEEKBAR, "[5s ►►]", C(BLK,LGY));
}

static void draw_info(void)
{
    mp_fill(0, ROW_INFO, MP_COLS, ' ', C(LGY,BLK));
    const char *name="(no file)", *fmts="";
    if (g_path[0]) {
        const char *p=g_path, *last=p;
        while (*p) { if (*p=='/') last=p+1; p++; }
        name=last;
        fmts=(g_fmt==FMT_WAV)?"[WAV]":(g_fmt==FMT_MP3)?"[MP3]":
             (g_fmt==FMT_AAC)?"[AAC]":"[???]";
    }
    mp_put(0, ROW_INFO, fmts, C(LBL,BLK));
    char buf[52]; int n=0;
    while (name[n]&&n<50){buf[n]=name[n];n++;} buf[n]=0;
    mp_put(6, ROW_INFO, buf, C(WHT,BLK));
}

static void draw_status(void)
{
    mp_fill(0, ROW_STATUS, MP_COLS, ' ', C(DGY,BLK));
    const char *ss=(g_state==ST_PLAYING)?"► PLAYING":
                   (g_state==ST_PAUSED) ?"|| PAUSED":"■ STOPPED";
    uint8_t sc=(g_state==ST_PLAYING)?C(LGN,BLK):
               (g_state==ST_PAUSED) ?C(YLW,BLK):C(DGY,BLK);
    mp_put(0, ROW_STATUS, ss, sc);
    mp_put(10,ROW_STATUS, "  ", C(DGY,BLK));
    mp_put(12,ROW_STATUS, g_status, C(LGY,BLK));
}

/*
  Volume row:
   0-5    "Vol: ["
   6-25   bar (20 chars)
   26-32  "] 080% "
   34-37  "[- ]"   ← Vol-
   39-42  "[+ ]"   ← Vol+
   44     scroll hint "▲"
   73     "▲" playlist scroll up
   (playlist scroll handled by arrows in playlist area)
*/
static void draw_volbar(void)
{
    mp_fill(0, ROW_VOLBAR, MP_COLS, ' ', C(DGY,BLK));
    mp_put(0, ROW_VOLBAR, "Vol:[", C(DGY,BLK));
    int vw = (int)(g_volume * 20 / 100);
    terminal_setpos(5, ROW_VOLBAR);
    for (int i=0;i<20;i++) {
        terminal_setcolor((i<vw)?C(LGN,BLK):C(DGY,BLK));
        terminal_putchar(i<vw?'\xDB':'\xB0');
    }
    char vbuf[10];
    vbuf[0]=']'; vbuf[1]=' ';
    vbuf[2]=(char)('0'+g_volume/100);
    vbuf[3]=(char)('0'+(g_volume%100)/10);
    vbuf[4]=(char)('0'+g_volume%10);
    vbuf[5]='%'; vbuf[6]=0;
    mp_put(25, ROW_VOLBAR, vbuf, C(WHT,BLK));
    mp_put(33, ROW_VOLBAR, "[Vol-]", C(WHT,DGY));
    mp_put(40, ROW_VOLBAR, "[Vol+]", C(WHT,DGY));
    mp_put(48, ROW_VOLBAR, "[▲ Scroll]", C(DGY,BLK));
    mp_put(59, ROW_VOLBAR, "[▼ Scroll]", C(DGY,BLK));
}

static void draw_playlist(void)
{
    draw_divider(ROW_DIV2, " Playlist (click to play) ");
    for (int r=0; r<PLIST_ROWS; r++) {
        int idx = g_plist_view + r;
        if (idx >= g_plist_n) {
            mp_fill(0, ROW_PLIST+r, MP_COLS, ' ', C(DGY,BLK));
            continue;
        }
        const char *p=g_plist[idx], *last=p;
        while (*p) { if (*p=='/') last=p+1; p++; }

        uint8_t col = (idx==g_plist_sel) ? C(BLK,LGN) : C(WHT,BLK);

        char line[MP_COLS+1]; int li=0;
        line[li++]=(idx==g_plist_sel)?'\x10':' '; /* ► or space */
        line[li++]=(char)('0'+(idx+1)/10);
        line[li++]=(char)('0'+(idx+1)%10);
        line[li++]='.'; line[li++]=' ';
        while (*last && li<MP_COLS-1) line[li++]=*last++;
        while (li<MP_COLS) line[li++]=' ';
        line[MP_COLS]=0;
        mp_put(0, ROW_PLIST+r, line, col);
    }
}

static void repaint(void)
{
    if (!g_dirty) return;
    if (g_dirty & DIRTY_TOOLBAR) draw_toolbar();
    if (g_dirty & DIRTY_DIVIDER) draw_divider(ROW_DIVIDER,
        g_path[0] ? g_path : "tOS Media Player " TOS_VERSION);
    if (g_dirty & DIRTY_SEEKBAR) draw_seekbar();
    if (g_dirty & DIRTY_INFO)    draw_info();
    if (g_dirty & DIRTY_STATUS)  draw_status();
    if (g_dirty & DIRTY_VOLBAR)  draw_volbar();
    if (g_dirty & DIRTY_PLIST)   draw_playlist();
    g_dirty = 0;
}

/* ── Click handlers ──────────────────────────────────────────────────────── */
static void click_toolbar(int cx)
{
    /* [► Play]/[||Pause]: col 1-10 */
    if (cx>=1 && cx<=10) {
        if (g_state==ST_PLAYING)     { audio_stop(); g_state=ST_PAUSED; }
        else if (g_state==ST_PAUSED) { g_state=ST_PLAYING; }
        else if (g_plist_n>0) { playlist_play_idx(g_plist_sel>=0?g_plist_sel:0); return; }
        g_dirty |= DIRTY_TOOLBAR|DIRTY_STATUS;
    }
    /* [■ Stop]: col 12-19 */
    else if (cx>=12 && cx<=19) {
        audio_stop(); g_state=ST_STOPPED; mp_free_file();
        strncpy(g_status,"Stopped.",MP_COLS);
        g_dirty = DIRTY_ALL;
    }
    /* [|◄ Prev]: col 22-29 */
    else if (cx>=22 && cx<=29) {
        if (g_plist_sel>0) playlist_play_idx(g_plist_sel-1);
    }
    /* [Next ►|]: col 32-39 */
    else if (cx>=32 && cx<=39) {
        if (g_plist_sel<g_plist_n-1) playlist_play_idx(g_plist_sel+1);
    }
    /* [LOOP]: col 42-51 */
    else if (cx>=42 && cx<=51) {
        g_loop=!g_loop;
        g_dirty |= DIRTY_TOOLBAR;
    }
}

static void click_seekbar(int cx)
{
    /* bar: col 1-48 */
    if (cx>=1 && cx<=48 && g_dur_sec) {
        uint32_t t=(uint32_t)((uint64_t)(cx-1)*g_dur_sec/47);
        if (g_fmt==FMT_WAV && g_filebuf)  wav_seek(&g_wav, t*AUDIO_OUT_RATE);
        else if (g_fmt==FMT_MP3)           mp3_seek_sec(&g_mp3, t);
        else if (g_fmt==FMT_WAV && !g_filebuf) {
            g_pcmpos=t*DEMO_SONG_RATE;
            if (g_pcmpos>DEMO_SONG_LEN) g_pcmpos=DEMO_SONG_LEN;
        }
        g_pcmfill=0; g_dirty|=DIRTY_SEEKBAR;
    }
    /* [◄◄ 5s]: col 65-71 */
    else if (cx>=65 && cx<=71) { do_seek_delta(-5); }
    /* [5s ►►]: col 73-79 */
    else if (cx>=73 && cx<=79) { do_seek_delta(5); }
}

static void click_volbar(int cx)
{
    /* [Vol-]: col 33-38 */
    if (cx>=33 && cx<=38) {
        if (g_volume>=5) g_volume-=5; else g_volume=0;
        audio_set_volume(g_volume);
        g_dirty|=DIRTY_VOLBAR;
    }
    /* [Vol+]: col 40-45 */
    else if (cx>=40 && cx<=45) {
        g_volume+=5; if (g_volume>100) g_volume=100;
        audio_set_volume(g_volume);
        g_dirty|=DIRTY_VOLBAR;
    }
    /* [▲ Scroll]: col 48-57 */
    else if (cx>=48 && cx<=57) {
        if (g_plist_view>0) { g_plist_view--; g_dirty|=DIRTY_PLIST; }
    }
    /* [▼ Scroll]: col 59-68 */
    else if (cx>=59 && cx<=68) {
        if (g_plist_view+PLIST_ROWS<g_plist_n) { g_plist_view++; g_dirty|=DIRTY_PLIST; }
    }
}

/* ── Keyboard shortcuts (secondary, non-blocking) ────────────────────────── */
static void handle_key(char k)
{
    switch(k) {
    case ' ':
        if (g_state==ST_PLAYING)     { audio_stop(); g_state=ST_PAUSED; g_dirty|=DIRTY_TOOLBAR|DIRTY_STATUS; }
        else if (g_state==ST_PAUSED) { g_state=ST_PLAYING; g_dirty|=DIRTY_TOOLBAR|DIRTY_STATUS; }
        else if (g_plist_n>0) { playlist_play_idx(g_plist_sel>=0?g_plist_sel:0); }
        break;
    case 27:
        audio_stop(); g_state=ST_STOPPED;
        strncpy(g_status,"Stopped.",MP_COLS);
        g_dirty=DIRTY_ALL;
        break;
    case '+': case '=':
        g_volume+=5; if(g_volume>100) g_volume=100;
        audio_set_volume(g_volume); g_dirty|=DIRTY_VOLBAR;
        break;
    case '-':
        if(g_volume>=5) g_volume-=5; else g_volume=0;
        audio_set_volume(g_volume); g_dirty|=DIRTY_VOLBAR;
        break;
    case 'n': case 'N':
        if (g_plist_sel<g_plist_n-1) playlist_play_idx(g_plist_sel+1);
        break;
    case 'p': case 'P':
        if (g_plist_sel>0) playlist_play_idx(g_plist_sel-1);
        break;
    case 'l': case 'L':
        g_loop=!g_loop; g_dirty|=DIRTY_TOOLBAR;
        break;
    }
}

static void handle_special_key(int spec)
{
    switch(spec) {
    case 1: do_seek_delta(-5); break;  /* left  */
    case 2: do_seek_delta(5);  break;  /* right */
    case 3: /* up: scroll playlist */
        if (g_plist_view>0) { g_plist_view--; g_dirty|=DIRTY_PLIST; } break;
    case 4: /* down: scroll playlist */
        if (g_plist_view+PLIST_ROWS<g_plist_n) { g_plist_view++; g_dirty|=DIRTY_PLIST; } break;
    }
}

/* ── Main entry point ────────────────────────────────────────────────────── */
void mediaplayer_open_path(const char *path)
{
    strncpy(g_pending_path, path, 63);
    g_has_pending = 1;
}

void mediaplayer_run(void)
{
    g_state=ST_STOPPED; g_fmt=FMT_NONE; g_filebuf=NULL; g_filesz=0;
    g_volume=80; g_loop=0; g_plist_n=0; g_plist_sel=-1; g_plist_view=0;
    g_pcmfill=0; g_pcmpos=0; g_path[0]=0;
    g_dirty=DIRTY_ALL; g_last_pos_sec=0xFFFFFFFFU; g_sw_tick=0;
    strncpy(g_status,"tOS Media Player - ready",MP_COLS);

    audio_init();
    audio_set_volume(g_volume);

    playlist_add("/demo/demo.wav");
    playlist_scan("/");

    if (g_has_pending) {
        g_has_pending=0;
        playlist_add(g_pending_path);
        playlist_play_idx(g_plist_n-1);
    } else {
        playlist_play_idx(0);
    }

    terminal_clear();
    repaint();

    for (;;) {
        gui_poll();

        /* ══ Audio pump + SW timer — run ALWAYS, focus-independent ══════ */
        if (g_state == ST_PLAYING) {

            /* ── Hardware audio pump ───────────────────────────────────── */
            if (audio_available() && !audio_busy()) {
                if (g_pcmfill == 0) {
                    mp_fill_pcm();
                    if (g_pcmfill == 0) {
                        /* EOF */
                        if (g_plist_sel>=0 && g_plist_sel<g_plist_n-1)
                            playlist_play_idx(g_plist_sel+1);
                        else if (g_loop)
                            playlist_play_idx(g_plist_sel);
                        else {
                            g_state=ST_STOPPED;
                            strncpy(g_status,"Playback complete.",MP_COLS);
                            g_dirty=DIRTY_ALL;
                        }
                    }
                }
                if (g_pcmfill > 0) {
                    audio_submit(g_pcmbuf, g_pcmfill);
                    g_pcmfill = 0;
                }
            }

            /* ── Software timer: ALWAYS advances position display ──────── *
             * Runs whether HW exists or not, whether HW is busy or not.   *
             * task_yield() ≈ 10ms → ~100 iters/s → SW_TICKS_PER_SEC=80  */
            g_sw_tick++;
            if (g_sw_tick >= SW_TICKS_PER_SEC) {
                g_sw_tick = 0;
                if (g_sw_pos_sec < g_dur_sec)
                    g_sw_pos_sec++;
                /* No-HW: drive pcmpos from SW timer so demo song "plays" */
                if (!audio_available()) {
                    if (g_fmt==FMT_WAV && !g_filebuf) {
                        g_pcmpos = g_sw_pos_sec * DEMO_SONG_RATE;
                        if (g_pcmpos >= DEMO_SONG_LEN) {
                            if (g_loop) { g_pcmpos=0; g_sw_pos_sec=0; }
                            else { g_state=ST_STOPPED;
                                   strncpy(g_status,"Playback complete.",MP_COLS);
                                   g_dirty=DIRTY_ALL; }
                        }
                    }
                } else if (g_sw_pos_sec >= g_dur_sec && g_dur_sec>0) {
                    /* HW mode: if SW timer ran out but pump didn't EOF yet, stop */
                    if (!g_loop) { g_state=ST_STOPPED;
                                   strncpy(g_status,"Playback complete.",MP_COLS);
                                   g_dirty=DIRTY_ALL; }
                    else { g_sw_pos_sec=0; }
                }
            }
        }

        /* ── Seek bar: redraw once per second ─────────────────────────── */
        {
            uint32_t pos = current_pos_sec();
            if (pos != g_last_pos_sec) {
                g_last_pos_sec = pos;
                g_dirty |= DIRTY_SEEKBAR;
            }
        }

        /* ══ UI — only when window has focus ════════════════════════════ */
        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        /* ── Mouse clicks ─────────────────────────────────────────────── */
        int cx,cy;
        if (wm_get_content_click(&cx,&cy)) {
            if (cy==ROW_TOOLBAR)
                click_toolbar(cx);
            else if (cy==ROW_SEEKBAR)
                click_seekbar(cx);
            else if (cy==ROW_VOLBAR)
                click_volbar(cx);
            else if (cy>=ROW_PLIST && cy<ROW_PLIST+PLIST_ROWS) {
                int idx=g_plist_view+(cy-ROW_PLIST);
                if (idx<g_plist_n) playlist_play_idx(idx);
            }
        }

        /* ── Keyboard (secondary) ─────────────────────────────────────── */
        int spec=keyboard_get_special();
        if (spec) handle_special_key(spec);
        if (keyboard_data_available()) handle_key(keyboard_getchar());

        repaint();
        task_yield();
    }
}
