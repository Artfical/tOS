#include "uiapp.h"
#include "uihost.h"
#include "string.h"
#include "memory.h"

/*
 * Files as the desktop handles them, shared by Files, the desktop and the Dock: what kind a file is and
 * its icon, opening it in the right app, moving and copying (between disks too, where a rename cannot
 * go), and the Trash (~/.Trash, like the Mac's), which deleting moves things to.
 */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int has_ext(const char *name, const char *exts)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    const char *e = exts;
    while (*e) {
        const char *w = e;
        while (*e && *e != ' ') e++;
        int len = (int)(e - w), ok = (int)strlen(dot + 1) == len;
        for (int i = 0; ok && i < len; i++) if (lower(dot[1 + i]) != w[i]) ok = 0;
        if (ok) return 1;
        while (*e == ' ') e++;
    }
    return 0;
}

#define IMG_EXT  "png jpg jpeg jfif"
#define TEXT_EXT "txt md c h py sh cfg conf log json ini csv ts rc"
#define AUD_EXT  "mp3 wav aac m4a"
#define WEB_EXT  "html htm svg"

const char *ui_file_kind(const char *name, int is_dir)
{
    if (is_dir) return "folder";
    if (has_ext(name, IMG_EXT)) return "image";
    if (has_ext(name, "pdf")) return "pdf";
    if (has_ext(name, AUD_EXT)) return "audio";
    if (has_ext(name, "t")) return "app";
    if (has_ext(name, WEB_EXT)) return "web";
    if (has_ext(name, TEXT_EXT) || !strchr(name, '.')) return "text";
    return "generic";
}

const gfx_image_t *ui_file_icon(const char *name, int is_dir, int big)
{
    char n[32];
    const char *k = ui_file_kind(name, is_dir);
    strcpy(n, big ? "doc64_" : "doc20_");
    strcat(n, !strcmp(k, "web") ? "text" : k);
    return gfx_image(n);
}

void ui_file_open(const char *path, int is_dir)
{
    if (is_dir) { uihost_open("files", path); return; }
    const char *k = ui_file_kind(ui_basename(path), 0);
    if (!strcmp(k, "image")) uihost_open("viewer", path);
    else if (!strcmp(k, "pdf")) uihost_open("pdf", path);
    else if (!strcmp(k, "audio")) uihost_open("music", path);
    else if (!strcmp(k, "app")) uihost_open("run", path);
    else if (!strcmp(k, "web")) uihost_open("tfire", path);
    else uihost_open("notepad", path);
}

/* ---- moving and copying ---- */

static void parent_of(const char *path, char *out)
{
    strcpy(out, path);
    int n = (int)strlen(out);
    while (n > 1 && out[n - 1] != '/') out[--n] = 0;
    if (n > 1) out[n - 1] = 0;
}

/* name in dir, or "name 2", "name 3"... (before the extension) when that is taken */
static void free_name(const char *dir, const char *name, char *out)
{
    ui_path_join(out, dir, name);
    if (!uihost_exists(out)) return;
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (dot == name) dot = 0;                              /* (.profile has no extension) */
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    for (int k = 2; k < 1000; k++) {
        char nm[80];
        int o = stem < 60 ? stem : 60;
        memcpy(nm, name, (size_t)o);
        nm[o] = 0;
        strcat(nm, " ");
        ui_num(nm, k);
        if (dot) strcat(nm, dot);
        ui_path_join(out, dir, nm);
        if (!uihost_exists(out)) return;
    }
}

static int copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > 16) return -1;
    if (uihost_exists(src) == 2) {
        if (uihost_mkdir(dst) != 0 && uihost_exists(dst) != 2) return -1;
        uihost_entry_t *ents = malloc(sizeof(uihost_entry_t) * 128);
        if (!ents) return -1;
        int n = uihost_list(src, ents, 128), r = n < 0 ? -1 : 0;
        for (int i = 0; i < n && r == 0; i++) {
            char a[224], b[224];
            ui_path_join(a, src, ents[i].name);
            ui_path_join(b, dst, ents[i].name);
            r = copy_tree(a, b, depth + 1);
        }
        free(ents);
        return r;
    }
    uint8_t *d;
    uint32_t len;
    if (uihost_read(src, &d, &len) != 0) return -1;
    int r = uihost_write(dst, d, len);
    free(d);
    return r;
}

int ui_file_delete(const char *path)
{
    if (uihost_exists(path) == 2) {
        uihost_entry_t *ents = malloc(sizeof(uihost_entry_t) * 128);
        if (!ents) return -1;
        /* a folder goes with everything in it, a batch at a time */
        for (int guard = 0; guard < 64; guard++) {
            int n = uihost_list(path, ents, 128);
            if (n <= 0) break;
            int gone = 0;
            for (int i = 0; i < n; i++) {
                char p[224];
                ui_path_join(p, path, ents[i].name);
                if (ui_file_delete(p) == 0) gone++;
            }
            if (!gone) break;
        }
        free(ents);
    }
    return uihost_delete(path);
}

int ui_file_move(const char *src, const char *dir, int copy, char *made, int cap)
{
    char from_dir[224], dst[224];
    parent_of(src, from_dir);
    int kind = uihost_exists(src);
    if (!kind) return -1;
    if (!copy && !strcmp(from_dir, dir)) { if (made) { strncpy(made, src, (size_t)cap - 1); made[cap - 1] = 0; } return 0; }
    /* not into itself */
    int sl = (int)strlen(src);
    if (kind == 2 && !strncmp(dir, src, (size_t)sl) && (dir[sl] == 0 || dir[sl] == '/')) return -1;
    free_name(dir, ui_basename(src), dst);
    int r = -1;
    if (!copy) r = uihost_rename(src, dst);                /* the same disk: just a new name */
    if (r != 0) {
        r = copy_tree(src, dst, 0);
        if (r == 0 && !copy) ui_file_delete(src);
        if (r != 0) ui_file_delete(dst);
    }
    if (r == 0 && made) { strncpy(made, dst, (size_t)cap - 1); made[cap - 1] = 0; }
    return r;
}

/* ---- the Trash ---- */

void ui_trash_dir(char *out)
{
    ui_path_join(out, uihost_home(), ".Trash");
    if (uihost_exists(out) != 2) uihost_mkdir(out);
}

int ui_file_trash(const char *path)
{
    char t[224];
    ui_trash_dir(t);
    int tl = (int)strlen(t);
    if (!strncmp(path, t, (size_t)tl) && (path[tl] == 0 || path[tl] == '/')) return 0;  /* don't silently erase from the Trash */
    return ui_file_move(path, t, 0, 0, 0);
}

int ui_trash_items(void)
{
    char t[224];
    ui_path_join(t, uihost_home(), ".Trash");
    if (uihost_exists(t) != 2) return 0;
    uihost_entry_t *ents = malloc(sizeof(uihost_entry_t) * 64);
    if (!ents) return 0;
    int n = uihost_list(t, ents, 64);
    free(ents);
    return n < 0 ? 0 : n;
}

int ui_trash_empty(void)
{
    char t[224];
    ui_trash_dir(t);
    uihost_entry_t *ents = malloc(sizeof(uihost_entry_t) * 128);
    if (!ents) return -1;
    int r = 0;
    for (int pass = 0; pass < 64; pass++) {
        int n = uihost_list(t, ents, 128);
        if (n < 0) { r = -1; break; }
        if (!n) break;
        int removed = 0;
        for (int i = 0; i < n; i++) {
            char p[224];
            ui_path_join(p, t, ents[i].name);
            if (ui_file_delete(p) == 0) removed++;
            else r = -1;
        }
        if (!removed) { r = -1; break; }
    }
    if (uihost_list(t, ents, 1) > 0) r = -1;
    free(ents);
    return r;
}
