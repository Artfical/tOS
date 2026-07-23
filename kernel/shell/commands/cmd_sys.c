#include "commands.h"
#include "terminal.h"
#include "string.h"
#include "version.h"
#include "io.h"
#include "klog.h"
#include "stdlib.h"
#include "memory.h"
#include "ramfs.h"
#include "scheduler.h"
#include "vga_font.h"
#include "sound.h"
#include "isr.h"
#include "keyboard.h"
#include "debugmon.h"
#include "audio.h"
#include "ich.h"
#include "bochs.h"
#include "vga.h"
#include "render3d.h"

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg | 0x80);
    return inb(0x71);
}

void cmd_help(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("tOS Commands:\n");
    terminal_writestring("  help       - show help\n");
    terminal_writestring("  man        - long-form manual (run 'man' for the list, 'man <cmd>' for details)\n");
    terminal_writestring("  echo       - echo text\n");
    terminal_writestring("  clear      - clear screen\n");
    terminal_writestring("  pwd        - print working directory\n");
    terminal_writestring("  ls         - list files\n");
    terminal_writestring("  cd         - change directory\n");
    terminal_writestring("  mkdir      - create directory\n");
    terminal_writestring("  rmdir      - remove directory\n");
    terminal_writestring("  rm         - remove file\n");
    terminal_writestring("  touch      - create file\n");
    terminal_writestring("  cat        - show file contents\n");
    terminal_writestring("  head       - show first lines of a file\n");
    terminal_writestring("  tail       - show last lines of a file\n");
    terminal_writestring("  wc         - word/line/char count\n");
    terminal_writestring("  sort       - sort lines alphabetically\n");
    terminal_writestring("  grep       - search pattern in file\n");
    terminal_writestring("  mv         - move/rename file\n");
    terminal_writestring("  cp         - copy file\n");
    terminal_writestring("  find       - find files in directory tree\n");
    terminal_writestring("  rev        - reverse characters of each line\n");
    terminal_writestring("  uniq       - filter adjacent duplicate lines\n");
    terminal_writestring("  edit       - simple line editor\n");
    terminal_writestring("  exec       - run ELF program\n");
    terminal_writestring("  tsharp     - run T# 4.1 Lite\n");
    terminal_writestring("  reboot     - reboot system\n");
    terminal_writestring("  shutdown   - halt system\n");
    terminal_writestring("  version    - show version\n");
    terminal_writestring("  about      - about tOS\n");
    terminal_writestring("  uname      - system info\n");
    terminal_writestring("  whoami     - current user\n");
    terminal_writestring("  hostname   - system hostname\n");
    terminal_writestring("  date       - show date/time\n");
    terminal_writestring("  cal        - show calendar\n");
    terminal_writestring("  df         - filesystem disk usage\n");
    terminal_writestring("  free       - memory usage\n");
    terminal_writestring("  dmesg      - kernel log messages\n");
    terminal_writestring("  ping       - ping a host\n");
    terminal_writestring("  ifconfig   - show NIC driver, MAC/IP, rx/tx counters\n");
    terminal_writestring("  wget       - download a file (HTTP)\n");
    terminal_writestring("  tpkg       - list/install packages from pkg.artfical.com\n");
    terminal_writestring("  yes        - print string repeatedly\n");
    terminal_writestring("  seq        - print number sequence\n");
    terminal_writestring("  sleep      - delay for N seconds\n");
    terminal_writestring("  basename   - strip directory from path\n");
    terminal_writestring("  dirname    - strip filename from path\n");
    terminal_writestring("  which      - locate a command\n");
    terminal_writestring("  env        - print environment\n");
    terminal_writestring("  uptime     - system uptime\n");
    terminal_writestring("  ps         - list processes\n");
    terminal_writestring("  log        - running tasks + kernel/operation log (disk ops, hex dumps)\n");
    terminal_writestring("  htop       - live process monitor\n");
    terminal_writestring("  disk       - manage disks (list/info/mount/umount/format)\n");
    terminal_writestring("  tar        - create/extract/list a tar archive (c/x/t)\n");
    terminal_writestring("  zip        - create a zip archive\n");
    terminal_writestring("  unzip      - extract a zip archive\n");
    terminal_writestring("  kill       - kill a process\n");
    terminal_writestring("  chmod      - change file mode\n");
    terminal_writestring("  hexdump    - hex dump of a file\n");
    terminal_writestring("  tee        - write stdin to file\n");
    terminal_writestring("  alias      - set/view aliases\n");
    terminal_writestring("  unalias    - remove alias\n");
    terminal_writestring("  history    - show command history\n");
    terminal_writestring("  python     - MicroPython REPL\n");
    terminal_writestring("  font       - list/change terminal font style\n");
}

void cmd_font(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("Available fonts:\n");
        for (int i = 0; i < VGA_FONT_STYLE_COUNT; i++) {
            terminal_writestring(i == vga_font_get_style() ? "* " : "  ");
            print_num(i);
            terminal_writestring(" - ");
            terminal_writestring(vga_font_style_name(i));
            terminal_putchar('\n');
        }
        terminal_writestring("usage: font <name|number>\n");
        return;
    }

    int idx = -1;
    if (args[1][0] >= '0' && args[1][0] <= '9') {
        idx = atoi(args[1]);
    } else {
        for (int i = 0; i < VGA_FONT_STYLE_COUNT; i++) {
            if (strcmp(args[1], vga_font_style_name(i)) == 0) { idx = i; break; }
        }
    }
    if (idx < 0 || idx >= VGA_FONT_STYLE_COUNT) {
        terminal_writestring("font: unknown font (run 'font' with no args to list)\n");
        return;
    }
    vga_font_set_style(idx);
    terminal_writestring("Font set to: ");
    terminal_writestring(vga_font_style_name(idx));
    terminal_putchar('\n');
}

void cmd_echo(int argc, char **args)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) terminal_putchar(' ');
        terminal_writestring(args[i]);
    }
    terminal_putchar('\n');
}

void cmd_clear(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_clear();
}

void cmd_version(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring(TOS_VERSION_STRING "\n");
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");
}

void cmd_beep(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(sound_get_enabled() ? "beep: on\n" : "beep: off\n");
        return;
    }
    if (strcmp(args[1], "on") == 0) {
        sound_set_enabled(1);
        terminal_writestring("beep: on\n");
    } else if (strcmp(args[1], "off") == 0) {
        sound_set_enabled(0);
        terminal_writestring("beep: off\n");
    } else if (strcmp(args[1], "test") == 0) {
        sound_click();
    } else {
        terminal_writestring("usage: beep [on|off|test]\n");
    }
}

static void print_hex16(uint16_t v)
{
    terminal_writestring("0x");
    for (int k = 12; k >= 0; k -= 4) terminal_putchar("0123456789ABCDEF"[(v >> k) & 0xF]);
}

/* Reports which audio backend (if any) is actually driving system
 * sounds, without needing to grep the kernel log -- forces detection
 * to run (audio_init() is otherwise only tried lazily on the first
 * beep) so this works even before any sound has played yet. */
void cmd_soundinfo(int argc, char **args)
{
    (void)argc; (void)args;
    audio_init();

    if (audio_available()) {
        terminal_writestring("Audio backend: ");
        terminal_writestring(audio_backend_name());
        terminal_writestring("\n");
        return;
    }

    terminal_writestring("Audio backend: none (falling back to PC speaker)\n");

    uint16_t vendor, device;
    if (ich_audio_get_unsupported(&vendor, &device)) {
        terminal_writestring("Found an unsupported audio device: vendor=");
        print_hex16(vendor);
        terminal_writestring(" device=");
        print_hex16(device);
        terminal_writestring("\n");
        terminal_writestring("(only Intel ICH AC97 is currently supported -- report this ID to add a driver for it)\n");
    } else {
        terminal_writestring("No PCI audio device found at all.\n");
    }
}

/* Temporary test command for the mode 13h VGA driver work: switches
 * to 320x200x256 graphics, draws a palette gradient bar plus a
 * crosshair so both pixel addressing and the DAC palette can be
 * checked visually, waits for a key, then switches back to text
 * mode. Not meant to ship long-term -- exists to verify vga_set_mode()
 * actually produces a correct picture before building anything on
 * top of it (e.g. a DOOM port). */
void cmd_vgatest(int argc, char **args)
{
    (void)argc; (void)args;

    /* Snapshot the current (real, valid) text-mode VGA registers
     * before anything touches VBE -- Bochs/VBE reprograms the legacy
     * CRTC/Sequencer/Graphics Controller registers to expose its
     * linear framebuffer, and disabling it later does not appear to
     * put those back, leaving the screen in a garbled in-between
     * state. Restoring this snapshot afterward is a second cleanup
     * step on top of bochs_disable(), not a replacement for it. */
    vga_init();

    bochs_device_t bochs;
    if (bochs_init(&bochs) != 0 || !bochs.lfb) {
        terminal_writestring("vgatest: no Bochs/VBE-capable display adapter found\n");
        return;
    }

    bochs_set_mode(&bochs, 320, 200, 32);

    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 320; x++) {
            uint32_t r = (uint32_t)(x * 255 / 320);
            uint32_t b = 255 - r;
            bochs_put_pixel(&bochs, x, y, (r << 16) | (0 << 8) | b);
        }
    }
    for (int x = 0; x < 320; x++) bochs_put_pixel(&bochs, x, 100, 0x000000);
    for (int y = 0; y < 200; y++) bochs_put_pixel(&bochs, 160, y, 0x000000);

    while (keyboard_data_available()) keyboard_getchar();
    keyboard_getchar();

    bochs_disable();
    vga_set_mode(VGA_MODE_TEXT);
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
}

/* Shows the exact same full-screen red panic display a real kernel
 * panic uses, with a made-up exception, then waits for any key and
 * returns to the shell instead of actually halting -- a demo/prank
 * command, not a real crash. */
void cmd_crash(int argc, char **args)
{
    (void)argc; (void)args;
    crash_screen_trigger("General Protection Fault", 0x0D, 0x00000000, 0xDEADC0DE, 0x00000000, 1);
    /* Drain whatever's still buffered from typing "crash" and hitting
     * Enter (scancode processing can leave a stray extra keystroke
     * queued) before waiting -- otherwise keyboard_getchar() below
     * returns instantly on that leftover input instead of a real key
     * press made after the screen is actually up. */
    while (keyboard_data_available()) keyboard_getchar();
    keyboard_getchar();
    crash_screen_clear();
    terminal_set_force_direct(0);
    terminal_setcolor(VGA_LIGHT_GREY | (VGA_BLACK << 4));
    terminal_clear();
    terminal_writestring("(That was a fake crash from the `crash` command.)\n");
}

/* Rotating, flat-shaded, z-buffered cube -- see render3d.c for the
 * actual rasterizer. Same real Bochs/VBE pixel path as `vgatest` and
 * `doom`, same one-way-trip-to-text-mode caveat if it doesn't shut
 * down cleanly (it does, via Escape). */
void cmd_3d(int argc, char **args)
{
    (void)argc; (void)args;
    render3d_run();
}

void cmd_about(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("tOS - talOS\n");
    terminal_writestring("License: GNU AGPL v3\n");
}

void cmd_uname(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("tOS\n");
}

void cmd_whoami(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("root\n");
}

void cmd_hostname(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("tOS\n");
}

void cmd_date(int argc, char **args)
{
    (void)argc; (void)args;
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);

    sec = (sec & 0x0F) + ((sec >> 4) * 10);
    min = (min & 0x0F) + ((min >> 4) * 10);
    hour = (hour & 0x0F) + ((hour >> 4) * 10);
    day = (day & 0x0F) + ((day >> 4) * 10);
    mon = (mon & 0x0F) + ((mon >> 4) * 10);
    year = (year & 0x0F) + ((year >> 4) * 10);

    terminal_writestring("20");
    print_num(year);
    terminal_putchar('-');
    if (mon < 10) terminal_putchar('0');
    print_num(mon);
    terminal_putchar('-');
    if (day < 10) terminal_putchar('0');
    print_num(day);
    terminal_putchar(' ');
    if (hour < 10) terminal_putchar('0');
    print_num(hour);
    terminal_putchar(':');
    if (min < 10) terminal_putchar('0');
    print_num(min);
    terminal_putchar(':');
    if (sec < 10) terminal_putchar('0');
    print_num(sec);
    terminal_putchar('\n');
}

static int day_of_week(int y, int m, int d)
{
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int days_in_month(int m, int y)
{
    static int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m - 1];
}

static const char *month_name(int m)
{
    static const char *names[] = {"January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    return names[m - 1];
}

void cmd_cal(int argc, char **args)
{
    (void)argc; (void)args;
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    mon = (mon & 0x0F) + ((mon >> 4) * 10);
    year = 2000 + ((year & 0x0F) + ((year >> 4) * 10));

    terminal_writestring(month_name(mon));
    terminal_putchar(' ');
    print_num(year);
    terminal_putchar('\n');
    terminal_writestring("Mo Tu We Th Fr Sa Su\n");

    int dow = day_of_week(year, mon, 1);
    int dim = days_in_month(mon, year);

    for (int i = 0; i < dow; i++)
        terminal_writestring("   ");

    for (int d = 1; d <= dim; d++) {
        if (d < 10) terminal_putchar(' ');
        print_num(d);
        terminal_putchar(' ');
        if ((dow + d) % 7 == 0) terminal_putchar('\n');
    }
    terminal_putchar('\n');
}

void cmd_df(int argc, char **args)
{
    (void)argc; (void)args;
    uint32_t total_inodes = 0;
    uint32_t used_inodes = 0;
    uint32_t total_size = 0;
    uint32_t used_size = 0;

    ramfs_get_usage(&used_inodes, &total_inodes, &used_size, &total_size);

    terminal_writestring("Filesystem     Inodes  Used  Free  Size\n");
    terminal_writestring("ramfs          ");
    print_num(total_inodes);
    terminal_putchar(' ');
    print_num(used_inodes);
    terminal_putchar(' ');
    print_num(total_inodes - used_inodes);
    terminal_putchar(' ');
    if (total_size >= 1024 * 1024) {
        print_num(total_size / (1024 * 1024));
        terminal_writestring(" MB\n");
    } else if (total_size >= 1024) {
        print_num(total_size / 1024);
        terminal_writestring(" KB\n");
    } else {
        print_num(total_size);
        terminal_writestring(" B\n");
    }
}

void cmd_free(int argc, char **args)
{
    (void)argc; (void)args;
    uint32_t total_kb = 0;
    uint32_t used_kb = 0;

    memory_get_usage(&total_kb, &used_kb);

    terminal_writestring("              total        used        free\n");
    terminal_writestring("Mem:          ");
    print_num(total_kb);
    terminal_writestring(" KB  ");
    print_num(used_kb);
    terminal_writestring(" KB  ");
    print_num(total_kb - used_kb);
    terminal_writestring(" KB\n");
}

void cmd_dmesg(int argc, char **args)
{
    (void)argc; (void)args;
    int len = 0;
    const char *log = klog_get(&len);
    if (len == 0) {
        terminal_writestring("dmesg: No kernel messages\n");
        return;
    }
    terminal_writestring(log);
}

void cmd_reboot(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("Rebooting...\n");
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    asm volatile("hlt");
}

void cmd_shutdown(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("System halted.\n");
    for (;;) { asm volatile("hlt"); }
}

void cmd_sleep(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: sleep <seconds>\n");
        return;
    }
    int sec = atoi(args[1]);
    if (sec <= 0 || sec > 3600) {
        terminal_writestring("sleep: invalid duration\n");
        return;
    }
    for (int s = 0; s < sec; s++) {
        uint32_t needed = 1193182;
        outb(0x43, 0x00);
        uint16_t start = inb(0x40);
        start |= inb(0x40) << 8;
        uint32_t elapsed = 0;
        uint16_t prev = start;
        while (elapsed < needed) {
            outb(0x43, 0x00);
            uint16_t curr = inb(0x40);
            curr |= inb(0x40) << 8;
            elapsed += (prev - curr) & 0xFFFF;
            prev = curr;
        }
    }
}

void cmd_uptime(int argc, char **args)
{
    (void)argc; (void)args;
    /* debugmon_uptime_ms() (PIT-register-calibrated TSC), not
     * task_get_ticks()/100 -- system_ticks advances on every
     * task_yield() software self-yield as well as real IRQ0 ticks, so
     * its rate depends on scheduling load rather than real wall-clock
     * time. */
    uint32_t ticks = task_get_ticks();
    uint32_t ms = debugmon_uptime_ms();
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    uint32_t hr = min / 60;
    sec %= 60; min %= 60;

    terminal_writestring("Uptime: ");
    if (hr > 0) { print_num(hr); terminal_writestring("h "); }
    if (min > 0) { print_num(min); terminal_writestring("m "); }
    print_num(sec); terminal_writestring("s (");
    print_num(ticks); terminal_writestring(" ticks)\n");
}

static void ps_callback(uint32_t pid, const char *name, uint32_t state)
{
    (void)name;
    terminal_putchar(' ');
    if (pid < 10) terminal_putchar(' ');
    if (pid < 100) terminal_putchar(' ');
    print_num(pid);
    terminal_writestring("  ");
    int nlen = 0;
    while (name[nlen]) nlen++;
    terminal_writestring(name);
    for (int i = nlen; i < 12; i++) terminal_putchar(' ');
    const char *s = "???";
    if (state == 0) s = "READY";
    else if (state == 1) s = "RUN";
    else if (state == 2) s = "SLP";
    else if (state == 3) s = "ZOMB";
    terminal_writestring(s);
    terminal_putchar('\n');
}

void cmd_ps(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring(" PID  NAME         STATE\n");
    task_foreach(ps_callback);
}

void cmd_log(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("=== Running tasks ===\n");
    terminal_writestring(" PID  NAME         STATE\n");
    task_foreach(ps_callback);

    terminal_writestring("\n=== Kernel / operation log (boot, disk ops, hex dumps) ===\n");
    int len = 0;
    const char *log = klog_get(&len);
    if (len == 0) terminal_writestring("(empty)\n");
    else terminal_writestring(log);
}

void cmd_kill(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: kill <pid>\n");
        return;
    }
    uint32_t pid = (uint32_t)atoi(args[1]);
    if (pid == 0 || pid == 1) {
        terminal_writestring("kill: Cannot kill that process\n");
        return;
    }
    if (task_kill(pid) == 0) {
        terminal_writestring("Process ");
        print_num(pid);
        terminal_writestring(" killed\n");
    } else {
        terminal_writestring("kill: No such process\n");
    }
}

void cmd_chmod(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: chmod <mode> <file>\n");
        terminal_writestring("  mode: 3-digit octal (e.g. 644, 755)\n");
        return;
    }
    uint32_t mode = 0;
    const char *m = args[1];
    while (*m >= '0' && *m <= '9') {
        mode = (mode << 3) | (unsigned long)(*m - '0');
        m++;
    }
    if (ramfs_exists(args[2])) {
        if (ramfs_is_dir(args[2]))
            mode |= S_IFDIR;
        else
            mode |= S_IFREG;
    }
    if (ramfs_chmod(args[2], mode) == 0) {
        terminal_writestring("Mode changed\n");
    } else {
        terminal_writestring("chmod: Failed\n");
    }
}


