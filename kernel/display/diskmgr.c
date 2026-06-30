#include "diskmgr.h"
#include "stdio.h"
#include "terminal.h"
#include "keyboard.h"
#include "scheduler.h"
#include "gui.h"
#include "wm.h"
#include "string.h"
#include "blockdev.h"
#include "diskops.h"

#define DM_COLS 60
#define DM_ROWS 15
#define BAR_W 40
#define ROW_H 2
#define LIST_Y0 2
#define STATUS_Y (DM_ROWS - 1)

static int selected = 0;
static char status_msg[DM_COLS + 1];

static uint8_t dm_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }

static void put_str(int x, int y, const char *s, uint8_t color)
{
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    while (*s) terminal_putchar(*s++);
}

static void put_char_at(int x, int y, char c, uint8_t color)
{
    if (x < 0 || x >= DM_COLS || y < 0 || y >= DM_ROWS) return;
    terminal_setcolor(color);
    terminal_setpos((size_t)x, (size_t)y);
    terminal_putchar(c);
}

/* Full-area blank, used only once on window open. The list/status rows are
 * always rewritten full-width in a single pass below, so a periodic clear
 * before redraw isn't needed — that two-pass clear-then-redraw is what
 * caused the visible flicker (same root cause as about.c). */
static void clear_area(void)
{
    uint8_t bg = dm_color(VGA_BLACK, VGA_LIGHT_GREY);
    for (int r = 0; r < DM_ROWS; r++) put_str(0, r, "                                                            ", bg);
}

static int fmt_uint(char *buf, unsigned int v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static const char *bd_type_name(blockdev_type_t t)
{
    switch (t) {
    case BLOCKDEV_ATA:     return "ATA";
    case BLOCKDEV_AHCI:    return "AHCI";
    case BLOCKDEV_NVME:    return "NVMe";
    case BLOCKDEV_USB_MSD: return "USB-MSD";
    default:               return "?";
    }
}

static uint32_t bd_size_mb(blockdev_t *bd)
{
    uint64_t bytes = bd->total_sectors * bd->sector_size;
    return (uint32_t)(bytes / (1024 * 1024));
}

static void prompt_line(const char *prompt, char *buf, int max)
{
    put_str(0, STATUS_Y, "                                                            ",
            dm_color(VGA_BLACK, VGA_LIGHT_GREY));
    put_str(0, STATUS_Y, prompt, dm_color(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)strlen(prompt), (size_t)STATUS_Y);
    keyboard_readline(buf, max);
}

static void draw_bar(int y, uint32_t size_mb, uint32_t max_mb)
{
    int filled = 0;
    if (max_mb > 0) filled = (int)(((uint64_t)size_mb * BAR_W) / max_mb);
    if (filled > BAR_W) filled = BAR_W;
    if (filled < 1 && size_mb > 0) filled = 1;

    for (int i = 0; i < BAR_W; i++) {
        if (i < filled)
            put_char_at(2 + i, y, (char)0xDB, dm_color(VGA_BLUE, VGA_LIGHT_GREY));
        else
            put_char_at(2 + i, y, (char)0xB0, dm_color(VGA_LIGHT_GREY, VGA_LIGHT_GREY));
    }
}

static void draw_list(void)
{
    put_str(0, 0, " Disk Utility - Up/Dn select, Enter mount/unmount, F format ",
            dm_color(VGA_WHITE, VGA_BLUE));

    int n = blockdev_count();
    if (n == 0) {
        char line[DM_COLS + 1];
        const char *msg = "No block devices found.";
        int k = 0;
        while (msg[k]) { line[k + 2] = msg[k]; k++; }
        line[0] = ' '; line[1] = ' ';
        k += 2;
        while (k < DM_COLS) line[k++] = ' ';
        line[k] = 0;
        put_str(0, LIST_Y0, line, dm_color(VGA_BLACK, VGA_LIGHT_GREY));
        return;
    }

    uint32_t max_mb = 0;
    for (int i = 0; i < n; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (bd && bd_size_mb(bd) > max_mb) max_mb = bd_size_mb(bd);
    }

    int max_rows = (STATUS_Y - LIST_Y0) / ROW_H;
    for (int i = 0; i < n && i < max_rows; i++) {
        blockdev_t *bd = blockdev_get(i);
        if (!bd) continue;
        int y = LIST_Y0 + i * ROW_H;
        int is_sel = (i == selected);

        char line[DM_COLS + 1];
        int k = 0;
        line[k++] = is_sel ? '>' : ' ';
        line[k++] = ' ';
        const char *name = bd->name;
        while (*name && k < 10) line[k++] = *name++;
        while (k < 11) line[k++] = ' ';
        const char *type = bd_type_name(bd->type);
        while (*type && k < 19) line[k++] = *type++;
        while (k < 20) line[k++] = ' ';
        k += fmt_uint(line + k, bd_size_mb(bd));
        line[k++] = 'M'; line[k++] = 'B'; line[k++] = ' ';
        while (k < 30) line[k++] = ' ';
        if (bd->mounted) {
            const char *m = bd->mount_point;
            const char *p = "mounted@";
            while (*p && k < DM_COLS) line[k++] = *p++;
            while (*m && k < DM_COLS) line[k++] = *m++;
        } else {
            const char *p = "unmounted";
            while (*p && k < DM_COLS) line[k++] = *p++;
        }
        while (k < DM_COLS) line[k++] = ' ';
        line[k] = 0;

        uint8_t color = is_sel ? dm_color(VGA_WHITE, VGA_BLUE) : dm_color(VGA_BLACK, VGA_LIGHT_GREY);
        put_str(0, y, line, color);

        draw_bar(y + 1, bd_size_mb(bd), max_mb);
    }
}

static void draw_status(void)
{
    char line[DM_COLS + 1];
    int k = 0;
    while (status_msg[k] && k < DM_COLS) { line[k] = status_msg[k]; k++; }
    while (k < DM_COLS) line[k++] = ' ';
    line[k] = 0;
    put_str(0, STATUS_Y, line, dm_color(VGA_BLACK, VGA_LIGHT_GREY));
}

static void do_toggle_mount(blockdev_t *bd)
{
    char err[80];
    if (bd->mounted) {
        if (diskops_umount(bd->mount_point, err, sizeof(err)) != 0) {
            strncpy(status_msg, err, sizeof(status_msg) - 1);
            status_msg[sizeof(status_msg) - 1] = 0;
        } else {
            strcpy(status_msg, "Unmounted.");
        }
        return;
    }

    char mp[64];
    prompt_line("Mount point: ", mp, sizeof(mp));
    if (!mp[0]) { status_msg[0] = 0; return; }

    char fstype[16];
    const char *detected = diskops_detect(bd->name);
    if (detected) {
        strncpy(fstype, detected, sizeof(fstype) - 1);
        fstype[sizeof(fstype) - 1] = 0;
    } else {
        prompt_line("Filesystem not auto-detected (tfsk/fat16/fat32/exfat/ext2/ext3/ext4/ntfs): ", fstype, sizeof(fstype));
        if (!fstype[0]) { status_msg[0] = 0; return; }
    }

    if (diskops_mount(bd->name, mp, fstype, err, sizeof(err)) != 0) {
        strncpy(status_msg, err, sizeof(status_msg) - 1);
        status_msg[sizeof(status_msg) - 1] = 0;
    } else if (detected) {
        snprintf(status_msg, sizeof(status_msg), "Mounted as %s (auto-detected).", fstype);
    } else {
        strcpy(status_msg, "Mounted.");
    }
}

static void do_format(blockdev_t *bd)
{
    if (bd->mounted) {
        strcpy(status_msg, "Unmount before formatting.");
        return;
    }

    char fstype[16];
    prompt_line("Format as (tfsk/fat16/fat32/exfat/ext2/ext3/ext4/ntfs): ", fstype, sizeof(fstype));
    if (!fstype[0]) { status_msg[0] = 0; return; }

    put_str(0, STATUS_Y, "                                                            ",
            dm_color(VGA_BLACK, VGA_LIGHT_GREY));
    char confirm[64];
    int k = 0;
    const char *p = "Erase ";
    while (*p) confirm[k++] = *p++;
    const char *n = bd->name;
    while (*n) confirm[k++] = *n++;
    p = "? (y/n) ";
    while (*p) confirm[k++] = *p++;
    confirm[k] = 0;
    put_str(0, STATUS_Y, confirm, dm_color(VGA_BLACK, VGA_LIGHT_GREY));
    terminal_setpos((size_t)k, (size_t)STATUS_Y);

    if (!keyboard_yesno()) { strcpy(status_msg, "Format cancelled."); return; }

    char err[80];
    if (diskops_format(bd->name, fstype, err, sizeof(err)) != 0) {
        strncpy(status_msg, err, sizeof(status_msg) - 1);
        status_msg[sizeof(status_msg) - 1] = 0;
    } else {
        strcpy(status_msg, "Format complete.");
    }
}

void diskmgr_run(void)
{
    selected = 0;
    status_msg[0] = 0;
    terminal_clear();
    clear_area();

    uint32_t last_redraw = 0;

    for (;;) {
        gui_poll();

        if (!wm_current_task_has_focus()) { task_yield(); continue; }

        int spec = keyboard_get_special();
        if (spec == 3) {
            if (selected > 0) selected--;
        } else if (spec == 4) {
            int n = blockdev_count();
            if (selected < n - 1) selected++;
        }

        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            int n = blockdev_count();
            blockdev_t *bd = (selected < n) ? blockdev_get(selected) : NULL;
            if (bd && (c == '\n' || c == '\r')) {
                do_toggle_mount(bd);
            } else if (bd && (c == 'f' || c == 'F')) {
                do_format(bd);
            }
        }

        uint32_t now = task_get_ticks();
        if (now - last_redraw >= 5) {
            last_redraw = now;
            draw_list();
            draw_status();
        }

        task_yield();
    }
}
