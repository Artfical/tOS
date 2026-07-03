#ifndef TOS_API_H
#define TOS_API_H

#include <stdint.h>

/* Single API surface shared by every scripting front-end (T#, embedded
 * MicroPython, and anything added later) so each language binding stays
 * a thin wrapper instead of reimplementing file/process/GUI access. */

/* Runs a shell command line (any builtin: ls, cat, mkdir, ping, disk,
 * ps, kill, ...) with its output captured into `out` instead of going
 * to the visible terminal. Always returns 0; a failing command's error
 * text (e.g. "No such file") ends up in `out` like any other output. */
int tos_exec(const char *cmd, char *out, int out_max);

/* Reads a whole file into `out` (NUL-terminated, truncated to out_max-1
 * bytes). Returns the number of bytes read, or -1 if the path doesn't
 * exist or is a directory. */
int tos_read(const char *path, char *out, int out_max);

/* Overwrites (or creates) a file with exactly `len` bytes. Returns 0 on
 * success, -1 on failure. */
int tos_write(const char *path, const char *data, int len);

int tos_mkdir(const char *path);
int tos_delete(const char *path);
int tos_exists(const char *path);

/* Lists a directory's entries (excluding "." and "..") as newline-
 * separated names into `out`. Returns the entry count, or -1 if the
 * path isn't a directory. */
int tos_list(const char *path, char *out, int out_max);

/* Opens a GUI app window by name: "notepad", "paint", "files", "viewer",
 * "calculator", "clock", "about", "diskutil", "taskmgr", "terminal".
 * Only meaningful in GUI mode. Returns 0 if the name was recognized,
 * -1 otherwise. */
int tos_open_app(const char *name);

/* Lists running tasks as "pid name state\n" lines into `out`. */
int tos_ps(char *out, int out_max);

/* Kills a task by pid, going through the same window-aware cleanup
 * Task Manager uses. Refuses the idle task and the caller's own task. */
int tos_kill(uint32_t pid);

/* Seconds since boot. */
uint32_t tos_uptime(void);

/* Fetches a URL over plain HTTP (only "http://" is supported — same
 * restriction as the `wget` shell command, since there's no TLS in
 * this network stack). Writes just the response *body* into `out`
 * (the status line and headers are parsed off and discarded), NUL-
 * terminated and truncated to out_max-1 bytes. Returns the body
 * length, or -1 on a DNS/connect/parse failure. */
int tos_http_get(const char *url, char *out, int out_max);

/* Audio playback — shared by T# and MicroPython, and by the file format
 * auto-detection Media Player already uses (WAV RIFF header / MP3 frame
 * sync or ID3 tag / M4A "moov" box vs. bare ADTS AAC). This call
 * BLOCKS until the whole file has finished playing (same blocking
 * model as tos_exec() — no background task involved), decoding and
 * submitting PCM chunk by chunk through whatever backend Media Player
 * uses (SB16/AC97), lazily initialized on first use if needed.
 * Returns 0 on success, -1 if the file doesn't exist, isn't a
 * recognized format, or no sound card is present. */
int tos_play_file(const char *path);

/* Immediately silences whatever is currently playing (Media Player or
 * a tos_play_file() call from another task). */
void tos_stop_audio(void);

/* 0-100. */
void tos_set_volume(int vol);

/* Whether the audio backend is still draining a submitted PCM chunk —
 * mostly useful right after a play call, or to poll Media Player's
 * state from a script. */
int tos_audio_playing(void);

#endif
