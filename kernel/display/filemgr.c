#include "filemgr.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "memory.h"
#include "fsbridge.h"
#include "vfs.h"

#define FM_COLS 79
#define FM_ROWS 22

#define TOOLBAR_ROW 0
#define DISKBAR_ROW 1
#define PATH_ROW 2
#define LIST_Y0 3
#define STATUS_ROW (FM_ROWS - 1)
#define LIST_VISIBLE (STATUS_ROW - LIST_Y0)

#define MAX_ENTRIES 96
#define NUM_BUTTONS 8
#define MAX_MOUNTS_SHOWN 8
#define DOUBLECLICK_TICKS 25
#define TRASH_DIR "/trash"

static const char *btn_labels[NUM_BUTTONS] = {
    "Up", "New", "Copy", "Cut", "Paste", "Del", "Rename", "Open"
};
static int btn_x0[NUM_BUTTONS], btn_x1[NUM_BUTTONS];

static char mount_paths[MAX_MOUNTS_SHOWN][VFS_NAME_LEN];
static int mount_count;
static int mount_x0[MAX_MOUNTS_SHOWN], mount_x1[MAX_MOUNTS_SHOWN];

static char cur_path[256] = "/";
static vfs_entry_t entries[MAX_ENTRIES];
static int entry_count;
static int selected;
static int scroll_off;
static int multi_selected[MAX_ENTRIES];
static int select_anchor;

static char clipboard_paths[MAX_ENTRIES][256];
static int clipboard_count;
static int clipboard_cut;

static char status_msg[FM_COLS + 1];
static uint32_t last_click_tick;
static int last_click_idx = -1;

static uint8_t fm_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void clear_area(void)
{
    terminal_setcolor(fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    for (int r = 0; r < FM_ROWS; r++) {
        terminal_setpos(0, (size_t)r);
        for (int c = 0; c < FM_COLS; c++) terminal_putchar(' ');
    }
}

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < FM_COLS) { status_msg[i] = s[i]; i++; }
    status_msg[i] = 0;
}

static int fmt_uint(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void join_path(const char *dir, const char *name, char *out, int max)
{
    int k = 0, i = 0;
    while (dir[i] && k < max - 1) { out[k++] = dir[i++]; }
    if (k == 0 || out[k - 1] != '/') { if (k < max - 1) out[k++] = '/'; }
    i = 0;
    while (name[i] && k < max - 1) out[k++] = name[i++];
    out[k] = 0;
}

static void path_up(char *path)
{
    int len = (int)strlen(path);
    if (len <= 1) return;
    int i = len - 1;
    while (i > 0 && path[i] != '/') i--;
    if (i == 0) path[1] = 0;
    else path[i] = 0;
}

static int has_suffix(const char *name, const char *suffix)
{
    int nlen = (int)strlen(name);
    int slen = (int)strlen(suffix);
    if (slen > nlen) return 0;
    return strcmp(name + (nlen - slen), suffix) == 0;
}

static const char *base_name(const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

static void clear_multi_select(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) multi_selected[i] = 0;
}

static int count_multi_selected(void)
{
    int n = 0;
    for (int i = 0; i < entry_count; i++) if (multi_selected[i]) n++;
    return n;
}

static int in_trash(void)
{
    int len = (int)strlen(TRASH_DIR);
    if (strncmp(cur_path, TRASH_DIR, (size_t)len) != 0) return 0;
    return cur_path[len] == 0 || cur_path[len] == '/';
}

/* Builds a collision-free destination path for `name` inside dir, trying
 * "name", then "name_1", "name_2", ... until one doesn't exist. */
static void unique_dest(const char *dir, const char *name, char *out, int max)
{
    join_path(dir, name, out, max);
    if (!fsbridge_exists(out)) return;
    for (int n = 1; n < 1000; n++) {
        char tagged[256];
        int k = 0;
        while (name[k] && k < 200) { tagged[k] = name[k]; k++; }
        tagged[k++] = '_';
        k += fmt_uint(tagged + k, (uint32_t)n);
        tagged[k] = 0;
        join_path(dir, tagged, out, max);
        if (!fsbridge_exists(out)) return;
    }
}

static int is_clipboard_cut_entry(const char *full)
{
    if (!clipboard_cut) return 0;
    for (int i = 0; i < clipboard_count; i++)
        if (strcmp(full, clipboard_paths[i]) == 0) return 1;
    return 0;
}

static void fix_scroll(void)
{
    if (selected < 0) selected = 0;
    if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;
    if (selected < scroll_off) scroll_off = selected;
    if (selected >= scroll_off + LIST_VISIBLE) scroll_off = selected - LIST_VISIBLE + 1;
    if (scroll_off < 0) scroll_off = 0;
}

static void refresh_list(void)
{
    vfs_entry_t raw[MAX_ENTRIES];
    int n = fsbridge_list(cur_path, raw, MAX_ENTRIES);
    entry_count = 0;
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0) continue;
            if (entry_count < MAX_ENTRIES) entries[entry_count++] = raw[i];
        }
    }
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = 0; j < entry_count - 1 - i; j++) {
            int swap = 0;
            if (entries[j].is_dir != entries[j + 1].is_dir) {
                if (!entries[j].is_dir && entries[j + 1].is_dir) swap = 1;
            } else if (strcmp(entries[j].name, entries[j + 1].name) > 0) swap = 1;
            if (swap) { vfs_entry_t t = entries[j]; entries[j] = entries[j + 1]; entries[j + 1] = t; }
        }
    }
    fix_scroll();
    clear_multi_select();
    if (entry_count > 0) multi_selected[selected] = 1;
    select_anchor = selected;
}

static void prompt_line(const char *prompt, char *buf, int max)
{
    put_str(0, STATUS_ROW, "                                                                               ",
            fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_ROW, prompt, fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)strlen(prompt), (size_t)STATUS_ROW);
    keyboard_readline(buf, max);
}

/* The directory-entry buffer is heap-allocated rather than a local array:
 * these two functions recurse into subdirectories, and a ~96-entry array
 * held on the stack at every recursion level would exhaust a task's 32 KB
 * kernel stack after just two or three nested folders. */
static int copy_recursive(const char *src, const char *dst)
{
    if (fsbridge_is_dir(src)) {
        if (fsbridge_mkdir(dst) != 0 && !fsbridge_exists(dst)) return -1;
        vfs_entry_t *raw = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_ENTRIES);
        if (!raw) return -1;
        int n = fsbridge_list(src, raw, MAX_ENTRIES);
        int ok = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0) continue;
            char s_child[256], d_child[256];
            join_path(src, raw[i].name, s_child, sizeof(s_child));
            join_path(dst, raw[i].name, d_child, sizeof(d_child));
            if (copy_recursive(s_child, d_child) != 0) { ok = -1; break; }
        }
        free(raw);
        return ok;
    }

    uint32_t sz = fsbridge_size(src);
    char *buf = sz ? (char *)malloc(sz) : NULL;
    if (sz && !buf) return -1;
    if (sz) fsbridge_read(src, buf, sz, 0);
    if (fsbridge_exists(dst)) fsbridge_delete(dst);
    int ok = (fsbridge_create(dst) == 0);
    if (ok && sz) ok = (fsbridge_write(dst, buf, sz, 0) >= 0);
    if (buf) free(buf);
    return ok ? 0 : -1;
}

static int delete_recursive(const char *path)
{
    if (fsbridge_is_dir(path)) {
        vfs_entry_t *raw = (vfs_entry_t *)malloc(sizeof(vfs_entry_t) * MAX_ENTRIES);
        if (!raw) return -1;
        int n = fsbridge_list(path, raw, MAX_ENTRIES);
        int ok = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0) continue;
            char child[256];
            join_path(path, raw[i].name, child, sizeof(child));
            if (delete_recursive(child) != 0) { ok = -1; break; }
        }
        free(raw);
        if (ok != 0) return ok;
    }
    return fsbridge_delete(path);
}

static void do_up(void)
{
    path_up(cur_path);
    selected = 0;
    scroll_off = 0;
    refresh_list();
    status_msg[0] = 0;
}

static void do_open_selected(void)
{
    if (entry_count == 0) return;
    vfs_entry_t *e = &entries[selected];
    char full[256];
    join_path(cur_path, e->name, full, sizeof(full));
    if (e->is_dir) {
        strncpy(cur_path, full, sizeof(cur_path) - 1);
        cur_path[sizeof(cur_path) - 1] = 0;
        selected = 0;
        scroll_off = 0;
        refresh_list();
        status_msg[0] = 0;
    } else if (has_suffix(e->name, ".png")) {
        wm_open_viewer_file(full);
        set_status("Opened in Image Viewer.");
    } else if (has_suffix(e->name, ".pdf")) {
        wm_open_pdfviewer_file(full);
        set_status("Opened in PDF Viewer.");
    } else {
        wm_open_notepad_file(full);
        set_status("Opened in Notepad.");
    }
}

static void do_new_folder(void)
{
    char name[64];
    prompt_line("New folder name: ", name, sizeof(name));
    if (!name[0]) { status_msg[0] = 0; return; }
    char full[256];
    join_path(cur_path, name, full, sizeof(full));
    if (fsbridge_mkdir(full) != 0) set_status("New folder: failed.");
    else set_status("Folder created.");
    refresh_list();
}

static void do_copy(void)
{
    clipboard_count = 0;
    for (int i = 0; i < entry_count && clipboard_count < MAX_ENTRIES; i++) {
        if (multi_selected[i]) join_path(cur_path, entries[i].name, clipboard_paths[clipboard_count++], sizeof(clipboard_paths[0]));
    }
    if (clipboard_count == 0) { set_status("Nothing selected."); return; }
    clipboard_cut = 0;

    char msg[40];
    int k = 0;
    k += fmt_uint(msg + k, (uint32_t)clipboard_count);
    const char *p = " item(s) copied.";
    while (*p) msg[k++] = *p++;
    msg[k] = 0;
    set_status(msg);
}

static void do_cut(void)
{
    clipboard_count = 0;
    for (int i = 0; i < entry_count && clipboard_count < MAX_ENTRIES; i++) {
        if (multi_selected[i]) join_path(cur_path, entries[i].name, clipboard_paths[clipboard_count++], sizeof(clipboard_paths[0]));
    }
    if (clipboard_count == 0) { set_status("Nothing selected."); return; }
    clipboard_cut = 1;

    char msg[40];
    int k = 0;
    k += fmt_uint(msg + k, (uint32_t)clipboard_count);
    const char *p = " item(s) cut.";
    while (*p) msg[k++] = *p++;
    msg[k] = 0;
    set_status(msg);
}

static void do_paste(void)
{
    if (clipboard_count == 0) { set_status("Clipboard is empty."); return; }

    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < clipboard_count; i++) {
        char dst[256];
        join_path(cur_path, base_name(clipboard_paths[i]), dst, sizeof(dst));
        if (fsbridge_exists(dst)) { fail_count++; continue; }

        int ok;
        if (clipboard_cut) {
            ok = (fsbridge_rename(clipboard_paths[i], dst) == 0);
            if (!ok) {
                ok = (copy_recursive(clipboard_paths[i], dst) == 0);
                if (ok) delete_recursive(clipboard_paths[i]);
            }
        } else {
            ok = (copy_recursive(clipboard_paths[i], dst) == 0);
        }
        if (ok) ok_count++; else fail_count++;
    }
    if (clipboard_cut) clipboard_count = 0;

    char msg[60];
    int k = 0;
    k += fmt_uint(msg + k, (uint32_t)ok_count);
    const char *p = " pasted";
    while (*p) msg[k++] = *p++;
    if (fail_count > 0) {
        p = ", "; while (*p) msg[k++] = *p++;
        k += fmt_uint(msg + k, (uint32_t)fail_count);
        p = " failed"; while (*p) msg[k++] = *p++;
    }
    msg[k++] = '.';
    msg[k] = 0;
    set_status(msg);
    refresh_list();
}

static void do_delete(void)
{
    int n = count_multi_selected();
    if (n == 0) return;
    int permanent = in_trash();

    char confirm[200];
    int k = 0;
    const char *p = permanent ? "Permanently delete " : "Move to Trash: ";
    while (*p) confirm[k++] = *p++;
    if (n == 1) {
        for (int i = 0; i < entry_count; i++) {
            if (multi_selected[i]) {
                const char *nm = entries[i].name;
                confirm[k++] = '\'';
                while (*nm && k < 180) confirm[k++] = *nm++;
                confirm[k++] = '\'';
                break;
            }
        }
    } else {
        k += fmt_uint(confirm + k, (uint32_t)n);
        p = " items";
        while (*p) confirm[k++] = *p++;
    }
    p = "? (y/n) ";
    while (*p) confirm[k++] = *p++;
    confirm[k] = 0;
    put_str(0, STATUS_ROW, "                                                                               ",
            fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_ROW, confirm, fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)k, (size_t)STATUS_ROW);

    if (!keyboard_yesno()) { status_msg[0] = 0; return; }

    if (!permanent && !fsbridge_exists(TRASH_DIR)) fsbridge_mkdir(TRASH_DIR);

    int ok_count = 0, fail_count = 0;
    for (int i = 0; i < entry_count; i++) {
        if (!multi_selected[i]) continue;
        char full[256];
        join_path(cur_path, entries[i].name, full, sizeof(full));
        int ok;
        if (permanent) {
            ok = (delete_recursive(full) == 0);
        } else {
            char dst[256];
            unique_dest(TRASH_DIR, entries[i].name, dst, sizeof(dst));
            ok = (fsbridge_rename(full, dst) == 0);
            if (!ok) {
                ok = (copy_recursive(full, dst) == 0);
                if (ok) delete_recursive(full);
            }
        }
        if (ok) ok_count++; else fail_count++;
    }

    char msg[60];
    k = 0;
    k += fmt_uint(msg + k, (uint32_t)ok_count);
    p = permanent ? " deleted" : " moved to Trash";
    while (*p) msg[k++] = *p++;
    if (fail_count > 0) {
        p = ", "; while (*p) msg[k++] = *p++;
        k += fmt_uint(msg + k, (uint32_t)fail_count);
        p = " failed"; while (*p) msg[k++] = *p++;
    }
    msg[k++] = '.';
    msg[k] = 0;
    set_status(msg);
    refresh_list();
}

static void do_rename(void)
{
    if (entry_count == 0) return;
    char newname[64];
    prompt_line("Rename to: ", newname, sizeof(newname));
    if (!newname[0]) { status_msg[0] = 0; return; }
    char old_full[256], new_full[256];
    join_path(cur_path, entries[selected].name, old_full, sizeof(old_full));
    join_path(cur_path, newname, new_full, sizeof(new_full));
    if (fsbridge_rename(old_full, new_full) != 0) set_status("Rename failed.");
    else set_status("Renamed.");
    refresh_list();
}

static void switch_mount(const char *path)
{
    strncpy(cur_path, path, sizeof(cur_path) - 1);
    cur_path[sizeof(cur_path) - 1] = 0;
    selected = 0;
    scroll_off = 0;
    refresh_list();
    status_msg[0] = 0;
}

static void draw_toolbar(void)
{
    put_str(0, TOOLBAR_ROW, "                                                                               ",
            fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    int x = 1;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        const char *lbl = btn_labels[i];
        int len = (int)strlen(lbl);
        int w = len + 4;
        btn_x0[i] = x;
        btn_x1[i] = x + w - 1;

        terminal_setpos((size_t)x, TOOLBAR_ROW);
        terminal_setcolor(fm_color(VGA_WHITE, VGA_BLUE));
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 1;
    }
}

static void draw_diskbar(void)
{
    put_str(0, DISKBAR_ROW, "                                                                               ",
            fm_color(VGA_BLACK, VGA_LIGHT_GREY));
    mount_count = vfs_get_mounts(mount_paths, MAX_MOUNTS_SHOWN);

    int active = -1, active_len = -1;
    for (int i = 0; i < mount_count; i++) {
        int plen = (int)strlen(mount_paths[i]);
        int match = (strncmp(cur_path, mount_paths[i], (size_t)plen) == 0);
        if (match && plen > active_len) { active = i; active_len = plen; }
    }

    int x = 1;
    for (int i = 0; i < mount_count; i++) {
        const char *lbl = mount_paths[i];
        int len = (int)strlen(lbl);
        int w = len + 4;
        if (x + w >= FM_COLS) break;
        mount_x0[i] = x;
        mount_x1[i] = x + w - 1;

        uint8_t c = (i == active) ? fm_color(VGA_WHITE, VGA_BLUE) : fm_color(VGA_BLACK, VGA_LIGHT_GREY);
        terminal_setpos((size_t)x, DISKBAR_ROW);
        terminal_setcolor(c);
        terminal_putchar('[');
        terminal_putchar(' ');
        for (int j = 0; j < len; j++) terminal_putchar(lbl[j]);
        terminal_putchar(' ');
        terminal_putchar(']');

        x += w + 1;
    }
}

static void draw_path(void)
{
    char line[FM_COLS + 1];
    int k = 0;
    const char *p = "Path: ";
    while (*p) line[k++] = *p++;
    int i = 0;
    while (cur_path[i] && k < FM_COLS) line[k++] = cur_path[i++];
    while (k < FM_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, PATH_ROW, line, fm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
}

static void draw_list(void)
{
    for (int row = 0; row < LIST_VISIBLE; row++) {
        int idx = scroll_off + row;
        int y = LIST_Y0 + row;
        char line[FM_COLS + 1];
        for (int c = 0; c < FM_COLS; c++) line[c] = ' ';
        line[FM_COLS] = 0;

        if (idx < entry_count) {
            vfs_entry_t *e = &entries[idx];
            line[0] = multi_selected[idx] ? '>' : ' ';
            line[1] = ' ';
            line[2] = e->is_dir ? 'D' : 'F';
            line[3] = ' ';
            int k = 4;
            int j = 0;
            while (e->name[j] && k < FM_COLS - 14) line[k++] = e->name[j++];

            char right[16];
            int rk;
            if (e->is_dir) {
                const char *tag = "<DIR>";
                rk = 0;
                while (tag[rk]) { right[rk] = tag[rk]; rk++; }
            } else {
                rk = fmt_uint(right, e->size);
                right[rk++] = 'B';
            }
            right[rk] = 0;
            int rx = FM_COLS - rk - 1;
            if (rx > k) {
                for (int z = k; z < rx; z++) line[z] = ' ';
                for (int z = 0; z < rk; z++) line[rx + z] = right[z];
            }

            int is_clip = 0;
            if (clipboard_cut) {
                char full[256];
                join_path(cur_path, e->name, full, sizeof(full));
                is_clip = is_clipboard_cut_entry(full);
            }

            uint8_t color;
            if (multi_selected[idx]) color = fm_color(VGA_WHITE, VGA_BLUE);
            else if (is_clip) color = fm_color(VGA_DARK_GREY, VGA_LIGHT_GREY);
            else if (e->is_dir) color = fm_color(VGA_BLUE, VGA_LIGHT_GREY);
            else color = fm_color(VGA_BLACK, VGA_LIGHT_GREY);

            put_str(0, y, line, color);
        } else {
            put_str(0, y, line, fm_color(VGA_BLACK, VGA_LIGHT_GREY));
        }
    }

    if (entry_count == 0) {
        put_str(2, LIST_Y0, "(empty)", fm_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
    }
}

static void draw_status(void)
{
    char line[FM_COLS + 1];
    int k = 0;
    while (status_msg[k] && k < FM_COLS) { line[k] = status_msg[k]; k++; }
    while (k < FM_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_ROW, line, fm_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void redraw(void)
{
    draw_toolbar();
    draw_diskbar();
    draw_path();
    draw_list();
    draw_status();
}

static void handle_toolbar(int i)
{
    switch (i) {
        case 0: do_up(); break;
        case 1: do_new_folder(); break;
        case 2: do_copy(); break;
        case 3: do_cut(); break;
        case 4: do_paste(); break;
        case 5: do_delete(); break;
        case 6: do_rename(); break;
        case 7: do_open_selected(); break;
    }
}

void filemgr_run(void)
{
    strcpy(cur_path, "/");
    selected = 0;
    scroll_off = 0;
    clipboard_count = 0;
    status_msg[0] = 0;
    last_click_idx = -1;
    last_click_tick = 0;

    terminal_clear();
    clear_area();
    refresh_list();
    redraw();

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int ccx, ccy;
        if (wm_get_content_click(&ccx, &ccy)) {
            if (ccy == TOOLBAR_ROW) {
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (ccx >= btn_x0[i] && ccx <= btn_x1[i]) { handle_toolbar(i); break; }
                }
            } else if (ccy == DISKBAR_ROW) {
                for (int i = 0; i < mount_count; i++) {
                    if (ccx >= mount_x0[i] && ccx <= mount_x1[i]) { switch_mount(mount_paths[i]); break; }
                }
            } else if (ccy >= LIST_Y0 && ccy < STATUS_ROW) {
                int idx = scroll_off + (ccy - LIST_Y0);
                if (idx < entry_count) {
                    uint32_t now = task_get_ticks();
                    int shift = keyboard_shift_held();
                    int ctrl = keyboard_ctrl_held();

                    if (shift) {
                        int lo = idx < select_anchor ? idx : select_anchor;
                        int hi = idx < select_anchor ? select_anchor : idx;
                        for (int i = 0; i < entry_count; i++) multi_selected[i] = (i >= lo && i <= hi);
                        selected = idx;
                    } else if (ctrl) {
                        multi_selected[idx] = !multi_selected[idx];
                        selected = idx;
                        select_anchor = idx;
                    } else if (idx == last_click_idx && now - last_click_tick < DOUBLECLICK_TICKS) {
                        selected = idx;
                        do_open_selected();
                    } else {
                        clear_multi_select();
                        multi_selected[idx] = 1;
                        selected = idx;
                        select_anchor = idx;
                    }
                    last_click_idx = idx;
                    last_click_tick = now;
                }
            }
            redraw();
            continue;
        }

        int spec = keyboard_get_special();
        if (spec) {
            if (spec == 3) { if (selected > 0) selected--; fix_scroll(); }
            else if (spec == 4) { if (selected < entry_count - 1) selected++; fix_scroll(); }
            if (!keyboard_shift_held()) {
                clear_multi_select();
                if (entry_count > 0) multi_selected[selected] = 1;
                select_anchor = selected;
            } else if (entry_count > 0) {
                int lo = selected < select_anchor ? selected : select_anchor;
                int hi = selected < select_anchor ? select_anchor : selected;
                for (int i = 0; i < entry_count; i++) multi_selected[i] = (i >= lo && i <= hi);
            }
            redraw();
            continue;
        }

        if (!keyboard_data_available()) { task_yield(); continue; }

        char c = keyboard_getchar();
        if (c == '\n') { do_open_selected(); redraw(); }
        else if (c == '\b' || c == 127) { do_up(); redraw(); }
        else if (c == 'n' || c == 'N') { do_new_folder(); redraw(); }
        else if (c == 'c' || c == 'C') { do_copy(); redraw(); }
        else if (c == 'x' || c == 'X') { do_cut(); redraw(); }
        else if (c == 'v' || c == 'V') { do_paste(); redraw(); }
        else if (c == 'd' || c == 'D') { do_delete(); redraw(); }
        else if (c == 'r' || c == 'R') { do_rename(); redraw(); }
        else if (c == 't' || c == 'T') {
            if (!fsbridge_exists(TRASH_DIR)) fsbridge_mkdir(TRASH_DIR);
            strcpy(cur_path, TRASH_DIR);
            selected = 0;
            scroll_off = 0;
            refresh_list();
            set_status("Trash.");
            redraw();
        }
    }
}
