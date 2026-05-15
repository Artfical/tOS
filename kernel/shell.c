#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "string.h"
#include "memory.h"
#include "ramfs.h"
#include "elf.h"
#include "io.h"
#include "version.h"

#define MAX_ARGS 16
#define MAX_CMD_LEN 512
#define EDIT_LINE_LEN 512

static int parse_args(char *cmd, char **args)
{
    int argc = 0;
    char *p = cmd;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    args[argc++] = p;
    while (*p) {
        if (*p == ' ') {
            *p = '\0';
            p++;
            while (*p == ' ') p++;
            if (*p && argc < MAX_ARGS) {
                args[argc++] = p;
                continue;
            }
            break;
        }
        p++;
    }
    return argc;
}

static void cmd_yardim(void)
{
    terminal_writestring("tOS Komutlar:\n");
    terminal_writestring("  yardim     - yardim goster\n");
    terminal_writestring("  ses        - yazi yaz\n");
    terminal_writestring("  temiz      - ekrani temizle\n");
    terminal_writestring("  konum      - bulundugun yeri goster\n");
    terminal_writestring("  bak        - dosyalari listele\n");
    terminal_writestring("  git        - dizin degistir\n");
    terminal_writestring("  kur        - dizin olustur\n");
    terminal_writestring("  sok        - dizin sil\n");
    terminal_writestring("  vur        - dosya sil\n");
    terminal_writestring("  dokun      - dosya olustur\n");
    terminal_writestring("  oku        - dosya icerigini goster\n");
    terminal_writestring("  tasi       - dosya tasi/yeniden adlandir\n");
    terminal_writestring("  esle       - dosya kopyala\n");
    terminal_writestring("  ciz        - basit dosya editoru\n");
    terminal_writestring("  kos        - ELF programi calistir\n");
    terminal_writestring("  yenile     - sistemi yeniden baslat\n");
    terminal_writestring("  dur        - sistemi durdur\n");
    terminal_writestring("  bilgi      - surum bilgisi\n");
    terminal_writestring("  hakkinda   - tOS hakkinda\n");
    terminal_writestring("  kim        - sistem bilgisi\n");
}

static void cmd_ses(int argc, char **args)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) terminal_putchar(' ');
        terminal_writestring(args[i]);
    }
    terminal_putchar('\n');
}

static void cmd_temiz(void)
{
    terminal_clear();
}

static void cmd_konum(void)
{
    terminal_writestring(ramfs_getcwd());
    terminal_putchar('\n');
}

static void cmd_bak(int argc, char **args)
{
    const char *path = ramfs_getcwd();
    if (argc > 1) path = args[1];

    ramfs_entry_t entries[256];
    int count = ramfs_list(path, entries, 256);
    if (count < 0) {
        terminal_writestring("dizin bulunamadi: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (count == 0) {
        terminal_writestring("dosya yok.\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (entries[i].is_dir) terminal_writestring("[d] ");
        else terminal_writestring("    ");
        terminal_writestring(entries[i].name);
        if (!entries[i].is_dir) {
            terminal_writestring(" (");
            char buf[16];
            int di = 0;
            uint32_t sz = entries[i].size;
            if (sz >= 10000000) { buf[di++] = '0' + sz / 10000000; sz %= 10000000; }
            if (di > 0 || sz >= 1000000) { buf[di++] = '0' + sz / 1000000; sz %= 1000000; }
            if (di > 0 || sz >= 100000) { buf[di++] = '0' + sz / 100000; sz %= 100000; }
            if (di > 0 || sz >= 10000) { buf[di++] = '0' + sz / 10000; sz %= 10000; }
            if (di > 0 || sz >= 1000) { buf[di++] = '0' + sz / 1000; sz %= 1000; }
            if (di > 0 || sz >= 100) { buf[di++] = '0' + sz / 100; sz %= 100; }
            if (di > 0 || sz >= 10) { buf[di++] = '0' + sz / 10; sz %= 10; }
            buf[di++] = '0' + sz;
            buf[di] = '\0';
            terminal_writestring(buf);
            terminal_writestring(" B)");
        }
        terminal_putchar('\n');
    }
}

static void cmd_git(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring(ramfs_getcwd());
        terminal_putchar('\n');
        return;
    }
    if (ramfs_chdir(args[1]) != 0) {
        terminal_writestring("dizin bulunamadi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
    }
}

static void cmd_kur(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: kur <dizin_adi>\n");
        return;
    }
    if (ramfs_mkdir(args[1]) != 0) {
        terminal_writestring("dizin olusturulamadi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
    }
}

static void cmd_sok(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: sok <dizin_adi>\n");
        return;
    }
    if (ramfs_delete(args[1]) != 0) {
        terminal_writestring("dizin silinemedi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
    }
}

static void cmd_vur(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: vur <dosya_adi>\n");
        return;
    }
    if (ramfs_delete(args[1]) != 0) {
        terminal_writestring("dosya silinemedi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
    }
}

static void cmd_dokun(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: dokun <dosya_adi>\n");
        return;
    }
    if (ramfs_create(args[1]) != 0) {
        terminal_writestring("dosya olusturulamadi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
    }
}

static void cmd_oku(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: oku <dosya>\n");
        return;
    }
    if (!ramfs_exists(args[1])) {
        terminal_writestring("dosya bulunamadi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
        return;
    }
    if (ramfs_is_dir(args[1])) {
        terminal_writestring("bu bir dizin: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
        return;
    }
    uint32_t sz = ramfs_size(args[1]);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        terminal_writestring("bellek yetersiz\n");
        return;
    }
    ramfs_read(args[1], buf, sz, 0);
    buf[sz] = '\0';
    terminal_writestring(buf);
    if (sz > 0 && buf[sz - 1] != '\n')
        terminal_putchar('\n');
    free(buf);
}

static void cmd_tasi(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("kullanimi: tasi <kaynak> <hedef>\n");
        return;
    }
    if (ramfs_rename(args[1], args[2]) != 0) {
        terminal_writestring("tasinamadi\n");
    }
}

static void cmd_esle(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("kullanimi: esle <kaynak> <hedef>\n");
        return;
    }
    if (!ramfs_exists(args[1]) || ramfs_is_dir(args[1])) {
        terminal_writestring("kaynak bulunamadi veya bir dizin\n");
        return;
    }
    if (ramfs_exists(args[2])) {
        terminal_writestring("hedef zaten var\n");
        return;
    }
    if (ramfs_create(args[2]) != 0) {
        terminal_writestring("hedef olusturulamadi\n");
        return;
    }
    uint32_t sz = ramfs_size(args[1]);
    char *buf = (char *)malloc(sz);
    if (!buf) {
        terminal_writestring("bellek yetersiz\n");
        ramfs_delete(args[2]);
        return;
    }
    ramfs_read(args[1], buf, sz, 0);
    ramfs_write(args[2], buf, sz, 0);
    free(buf);
}

static void cmd_ciz(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: ciz <dosya>\n");
        terminal_writestring("yeni satir eklemek icin her satirdan sonra enter\n");
        terminal_writestring("kaydetmek icin .s yazip enter\n");
        terminal_writestring("cikmak icin .q yazip enter\n");
        return;
    }
    const char *filename = args[1];
    int exists = ramfs_exists(filename);
    if (!exists) {
        if (ramfs_create(filename) != 0) {
            terminal_writestring("dosya olusturulamadi\n");
            return;
        }
    }

    terminal_writestring("ciz: ");
    terminal_writestring(filename);
    terminal_writestring(" - satir ekle (.s=kaydet, .q=cik)\n");

    char line[EDIT_LINE_LEN];
    uint32_t offset = 0;
    if (exists) offset = ramfs_size(filename);

    for (;;) {
        terminal_writestring("> ");
        keyboard_readline(line, EDIT_LINE_LEN);

        if (strcmp(line, ".q") == 0) {
            terminal_writestring("kaydedilmedi.\n");
            if (!exists) ramfs_delete(filename);
            return;
        }
        if (strcmp(line, ".s") == 0) {
            terminal_writestring("kaydedildi.\n");
            return;
        }

        ramfs_write(filename, line, strlen(line), offset);
        ramfs_write(filename, "\n", 1, offset + strlen(line));
        offset += strlen(line) + 1;
    }
}

static void cmd_kos(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("kullanimi: kos <program>\n");
        return;
    }
    if (!ramfs_exists(args[1]) || ramfs_is_dir(args[1])) {
        terminal_writestring("program bulunamadi: ");
        terminal_writestring(args[1]);
        terminal_putchar('\n');
        return;
    }

    uint32_t sz = ramfs_size(args[1]);
    void *prog = malloc(sz);
    if (!prog) {
        terminal_writestring("bellek yetersiz\n");
        return;
    }
    ramfs_read(args[1], prog, sz, 0);

    terminal_writestring("yukleniyor: ");
    terminal_writestring(args[1]);
    terminal_putchar('\n');

    uint32_t entry = 0;
    if (elf_load(prog, &entry) != 0) {
        terminal_writestring("ELF yuklenemedi\n");
        free(prog);
        return;
    }

    char buf[16];
    for (int i = 0; i < 8; i++) {
        buf[7 - i] = "0123456789ABCDEF"[entry & 0xF];
        entry >>= 4;
    }
    buf[8] = '\0';
    terminal_writestring("giris: 0x");
    terminal_writestring(buf);
    terminal_putchar('\n');
    free(prog);
}

static void cmd_yenile(void)
{
    terminal_writestring("yeniden baslatiliyor...\n");
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    asm volatile("hlt");
}

static void cmd_dur(void)
{
    terminal_writestring("sistem durduruldu.\n");
    for (;;) { asm volatile("hlt"); }
}

static void cmd_bilgi(void)
{
    terminal_writestring(TOS_VERSION_STRING "\n");
    terminal_writestring("derleme: " __DATE__ " " __TIME__ "\n");
}

static void cmd_hakkinda(void)
{
    terminal_writestring("tOS - talOS\n");
    terminal_writestring("Lisans: GNU AGPL v3\n");
}

static void cmd_kim(void)
{
    terminal_writestring("tOS\n");
}

void shell_init(void)
{
    terminal_writestring(TOS_WELCOME_STRING);
    terminal_writestring("yardim yazarak komutlari gorebilirsin\n\n");
}

void shell_run(void)
{
    char cmd_line[MAX_CMD_LEN];
    char *args[MAX_ARGS];

    for (;;) {
        terminal_writestring(ramfs_getcwd());
        terminal_writestring("> ");

        keyboard_readline(cmd_line, MAX_CMD_LEN);

        int argc = parse_args(cmd_line, args);

        if (argc == 0) continue;

        const char *c = args[0];

        if (strcmp(c, "yardim") == 0) {
            cmd_yardim();
        } else if (strcmp(c, "ses") == 0) {
            cmd_ses(argc, args);
        } else if (strcmp(c, "temiz") == 0) {
            cmd_temiz();
        } else if (strcmp(c, "konum") == 0) {
            cmd_konum();
        } else if (strcmp(c, "bak") == 0) {
            cmd_bak(argc, args);
        } else if (strcmp(c, "git") == 0) {
            cmd_git(argc, args);
        } else if (strcmp(c, "kur") == 0) {
            cmd_kur(argc, args);
        } else if (strcmp(c, "sok") == 0) {
            cmd_sok(argc, args);
        } else if (strcmp(c, "vur") == 0) {
            cmd_vur(argc, args);
        } else if (strcmp(c, "dokun") == 0) {
            cmd_dokun(argc, args);
        } else if (strcmp(c, "oku") == 0) {
            cmd_oku(argc, args);
        } else if (strcmp(c, "tasi") == 0) {
            cmd_tasi(argc, args);
        } else if (strcmp(c, "esle") == 0) {
            cmd_esle(argc, args);
        } else if (strcmp(c, "ciz") == 0) {
            cmd_ciz(argc, args);
        } else if (strcmp(c, "kos") == 0) {
            cmd_kos(argc, args);
        } else if (strcmp(c, "yenile") == 0) {
            cmd_yenile();
        } else if (strcmp(c, "dur") == 0) {
            cmd_dur();
        } else if (strcmp(c, "bilgi") == 0) {
            cmd_bilgi();
        } else if (strcmp(c, "hakkinda") == 0) {
            cmd_hakkinda();
        } else if (strcmp(c, "kim") == 0) {
            cmd_kim();
        } else {
            terminal_writestring("bilinmeyen komut: ");
            terminal_writestring(c);
            terminal_putchar('\n');
        }
    }
}
