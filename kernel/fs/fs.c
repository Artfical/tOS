#include "fs.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"

static uint32_t initrd_addr = 0;
static uint32_t initrd_size = 0;
static fs_file_t file_table[FS_MAX_FILES];
static int file_count = 0;

static unsigned long octal_to_ulong(const char *str, int len)
{
    unsigned long result = 0;
    for (int i = 0; i < len && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7')
            result = (result << 3) | (str[i] - '0');
        else
            break;
    }
    return result;
}

void fs_init(uint32_t addr, uint32_t size)
{
    initrd_addr = addr;
    initrd_size = size;
    file_count = 0;

    memset(file_table, 0, sizeof(file_table));

    uint8_t *data = (uint8_t *)addr;
    if (size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        terminal_writestring("Initrd: raw ELF detected\n");
        return;
    }

    uint32_t pos = 0;
    while (pos + 512 <= size) {
        char *name = (char *)(data + pos);
        if (name[0] == '\0') break;

        char *magic = (char *)(data + pos + 257);
        if (memcmp(magic, "ustar", 5) != 0) break;

        char *size_str = (char *)(data + pos + 124);
        uint32_t file_size = (uint32_t)octal_to_ulong(size_str, 11);

        if (name[0] != '\0') {
            if (memcmp(name, "./", 2) == 0) {
                memmove(name, name + 2, strlen(name + 2) + 1);
            }

            if (strlen(name) > 0 && file_size > 0) {
                if (file_count < FS_MAX_FILES) {
                    strncpy(file_table[file_count].name, name, FS_NAME_LEN - 1);
                    file_table[file_count].size = file_size;
                    file_table[file_count].offset = pos + 512;
                    file_table[file_count].exists = 1;
                    file_count++;
                }
            }
        }

        uint32_t data_blocks = (file_size + 511) / 512;
        pos += 512 + data_blocks * 512;
    }

    char buf[32];
    int di = 0;
    int fc = file_count;
    if (fc >= 100) { buf[di++] = '0' + fc / 100; fc %= 100; }
    if (di > 0 || fc >= 10) { buf[di++] = '0' + fc / 10; fc %= 10; }
    buf[di++] = '0' + fc;
    buf[di] = '\0';
    terminal_writestring("Initrd loaded: ");
    terminal_writestring(buf);
    terminal_writestring(" files\n");
}

int fs_list(fs_file_t *files, int max)
{
    int count = 0;
    for (int i = 0; i < file_count && count < max; i++) {
        if (file_table[i].exists) {
            files[count] = file_table[i];
            count++;
        }
    }
    return count;
}

int fs_open(const char *name, fs_file_t *file)
{
    for (int i = 0; i < file_count; i++) {
        if (file_table[i].exists && strcmp(file_table[i].name, name) == 0) {
            *file = file_table[i];
            return 0;
        }
    }
    return -1;
}

int fs_read(fs_file_t *file, void *buf, uint32_t size, uint32_t offset)
{
    if (!file->exists) return -1;
    if (offset >= file->size) return 0;
    uint32_t readable = file->size - offset;
    if (size > readable) size = readable;
    memcpy(buf, (void *)(initrd_addr + file->offset + offset), size);
    return size;
}

int fs_exists(const char *name)
{
    for (int i = 0; i < file_count; i++) {
        if (file_table[i].exists && strcmp(file_table[i].name, name) == 0)
            return 1;
    }
    return 0;
}
