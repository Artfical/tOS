#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/*
 * tOS Recovery, the way a Mac starts from its recovery system: one window with the choices
 * (Install tOS, Disk Utility, Try tOS in Live Environment), and for installing the steps of a
 * setup assistant: the keyboard (with a picture of it), the network, the account with its
 * picture, the disk, then the installation itself. Integer arithmetic only.
 */

enum { P_HOME, P_KEYBOARD, P_NETWORK, P_ACCOUNT, P_DISK, P_INSTALL };
enum { F_FULL, F_USER, F_PASS, F_VERIFY, F_COUNT };

typedef struct {
    int page, choice, trying;          /* trying: the keyboard page leads to the live session */
    int layout;
    char test[48];                     /* "type here to try your keyboard" */
    int last_key;
    char field[F_COUNT][48];
    int focus, user_edited, picture;
    char err[96];
    uihost_disk_t disks[8];
    int nd, disk;
    int installing, pct, result;
    char status[96];
} rc_t;

#define BTN_W 110
#define BTN_H 32

/* ------------------------------------------------------------------ small pieces */

static void title(gfx_surface_t *s, int x, int y, int w, const char *t, const char *sub)
{
    gfx_text_center(s, &ui_font_title, x, y, w, t, UI_TEXT, 255);
    if (sub) gfx_text_center(s, &ui_font_regular, x, y + 34, w, sub, UI_DIM, 255);
}

static void bottom_buttons(gfx_surface_t *s, int x, int y, int w, int h, int back, const char *next, int next_ok)
{
    (void)w;
    if (back) ui_button(s, x + 20, y + h - 46, BTN_W, BTN_H, "Back", UI_CARD2, UI_TEXT, 0);
    if (next) ui_button(s, x + w - 20 - BTN_W, y + h - 46, BTN_W, BTN_H, next, next_ok ? UI_ACCENT : UI_CARD2, next_ok ? 0xFFFFFF : UI_DIM, 0);
}
static int hit_back(uiapp_t *a, int px, int py) { return ui_hit(px, py, 20, a->h - 46, BTN_W, BTN_H); }
static int hit_next(uiapp_t *a, int px, int py) { return ui_hit(px, py, a->w - 20 - BTN_W, a->h - 46, BTN_W, BTN_H); }

static void field_box(gfx_surface_t *s, int x, int y, int w, const char *label, const char *text, int secret, int focused)
{
    gfx_text(s, &ui_font_regular, x, y + 7, label, UI_TEXT, 255);
    int fx = x + 150, fw = w - 150;
    gfx_rrect(s, fx, y, fw, 30, 7, UI_FIELD, 255);
    gfx_rrect_outline(s, fx, y, fw, 30, 7, focused ? UI_ACCENT : UI_LINE, 255);
    char shown[48];
    int n = (int)strlen(text);
    if (secret) { for (int i = 0; i < n && i < 40; i++) shown[i] = '*'; shown[n < 40 ? n : 40] = 0; }
    else strcpy(shown, text);
    int tw = gfx_text(s, &ui_font_regular, fx + 10, y + 6, shown, UI_TEXT, 255);
    if (focused && (uihost_ms() / 500) % 2 == 0) gfx_fill(s, fx + 11 + tw, y + 6, 2, 18, UI_TEXT);
}

static void ip_text(char *o, uint32_t ip)
{
    o[0] = 0;
    for (int i = 0; i < 4; i++) { ui_num(o, (ip >> (8 * i)) & 255); if (i < 3) strcat(o, "."); }
}

/* The keyboard hands Turkish letters over as ISO-8859-9 (Latin-5): written out as UTF-8 here.
 * Returns the bytes written (0 for a key that is not a letter). */
static int latin5_utf8(int k, char *o)
{
    unsigned cp = (unsigned)k;
    switch (k) {
    case 0xD0: cp = 0x11E; break;   /* Ğ */
    case 0xDD: cp = 0x130; break;   /* İ */
    case 0xDE: cp = 0x15E; break;   /* Ş */
    case 0xF0: cp = 0x11F; break;   /* ğ */
    case 0xFD: cp = 0x131; break;   /* ı */
    case 0xFE: cp = 0x15F; break;   /* ş */
    }
    if (cp < 32 || cp == 127) return 0;
    if (cp < 128) { o[0] = (char)cp; return 1; }
    o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63));
    return 2;
}

static void text_append(char *t, int cap, int k)
{
    int n = (int)strlen(t);
    char u[2];
    int m = latin5_utf8(k, u);
    if (!m || n + m >= cap) return;
    memcpy(t + n, u, (size_t)m);
    t[n + m] = 0;
}

/* deletes the last character, all of its UTF-8 bytes */
static void text_backspace(char *t)
{
    int n = (int)strlen(t);
    while (n > 0 && ((unsigned char)t[n - 1] & 0xC0) == 0x80) t[--n] = 0;
    if (n > 0) t[n - 1] = 0;
}

/* ------------------------------------------------------------------ the keyboard picture */

/* five rows of keys; a label of "" is a wide key drawn with its name */
static const char *const kb_us[4][14] = {
    { "`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "delete" },
    { "tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\" },
    { "caps", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "return", 0 },
    { "shift", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "shift", 0, 0 },
};
static const char *const kb_tr[4][14] = {
    { "\"", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "*", "-", "delete" },
    { "tab", "q", "w", "e", "r", "t", "y", "u", "\xC4\xB1", "o", "p", "\xC4\x9F", "\xC3\xBC", "," },
    { "caps", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC5\x9F", "i", "return", 0 },
    { "shift", "z", "x", "c", "v", "b", "n", "m", "\xC3\xB6", "\xC3\xA7", ".", "shift", 0, 0 },
};

static int key_w(const char *k, int u)
{
    if (!strcmp(k, "delete") || !strcmp(k, "tab")) return u * 3 / 2;
    if (!strcmp(k, "caps")) return u * 7 / 4;
    if (!strcmp(k, "return")) return u * 7 / 4 + 4;
    if (!strcmp(k, "shift")) return u * 9 / 4 + 2;
    return u;
}

static void draw_keyboard(gfx_surface_t *s, int x, int y, int layout, int pressed)
{
    const char *const (*rows)[14] = layout ? kb_tr : kb_us;
    int u = 34, g = 4;
    int total = 0;
    for (int i = 0; i < 14; i++) total += key_w(rows[0][i], u) + g;
    gfx_rrect(s, x - 12, y - 12, total + 20, 5 * (u + g) + 20, 12, ui_theme.dark ? 0x2A2B30 : 0xD5D6DB, 255);
    for (int r = 0; r < 5; r++) {
        int kx = x, ky = y + r * (u + g);
        if (r == 4) {   /* the space bar row */
            static const char *const mods[3] = { "fn", "ctrl", "alt" };
            for (int i = 0; i < 3; i++) { gfx_rrect(s, kx, ky, u, u, 6, ui_theme.dark ? 0x45464E : 0xFFFFFF, 255); gfx_text_center(s, &ui_font_small, kx, ky + 10, u, mods[i], UI_DIM, 255); kx += u + g; }
            int sw = total - 3 * (u + g) - 3 * (u + g) - g;
            gfx_rrect(s, kx, ky, sw, u, 6, pressed == ' ' ? UI_ACCENT : (ui_theme.dark ? 0x45464E : 0xFFFFFF), 255);
            kx += sw + g;
            static const char *const arr[3] = { "\xE2\x86\x90", "\xE2\x86\x92", "alt" };
            for (int i = 0; i < 3; i++) { gfx_rrect(s, kx, ky, u, u, 6, ui_theme.dark ? 0x45464E : 0xFFFFFF, 255); gfx_text_center(s, &ui_font_small, kx, ky + 10, u, arr[i], UI_DIM, 255); kx += u + g; }
            continue;
        }
        for (int i = 0; i < 14 && rows[r][i]; i++) {
            const char *k = rows[r][i];
            int w = key_w(k, u);
            if (r == 3 && i == 11) w = x + total - g - kx;           /* the right shift fills the row */
            if (r == 2 && i == 12) w = x + total - g - kx;           /* so does return */
            int single = strlen(k) <= 2 || (unsigned char)k[0] >= 0xC0;
            int on = single && pressed && (unsigned char)k[0] < 0x80 && k[0] == pressed;
            gfx_rrect(s, kx, ky, w, u, 6, on ? UI_ACCENT : (ui_theme.dark ? 0x45464E : 0xFFFFFF), 255);
            gfx_rrect_outline(s, kx, ky, w, u, 6, 0x000000, 25);
            if (single) gfx_text_center(s, &ui_font_medium, kx, ky + 8, w, k, on ? 0xFFFFFF : UI_TEXT, 255);
            else gfx_text_center(s, &ui_font_small, kx, ky + 10, w, k, UI_DIM, 255);
            kx += w + g;
        }
    }
}

/* ------------------------------------------------------------------ pages */

static const char *const choice_icon[3] = { "rec_install", "rec_diskutil", "rec_try" };
static const char *const choice_title[3] = { "Install tOS", "Disk Utility", "Try tOS in Live Environment" };
static const char *const choice_text[3] = {
    "Install tOS on a disk of this computer. The disk is erased.",
    "Repair a disk with First Aid, or erase it.",
    "Use tOS straight from here, without changing anything on this computer." };

/* the rows of the first page, the way the Mac's recovery lists its utilities */
#define HOME_ROW_Y(i) (60 + (i) * 104)

static void page_home(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    for (int i = 0; i < 3; i++) {
        int cy = y + HOME_ROW_Y(i), cx = x + 60, cw = w - 120;
        if (r->choice == i) gfx_rrect(s, cx, cy, cw, 92, 10, ui_theme.dark ? 0x3A3A40 : 0xE6E6EB, 255);
        gfx_blit(s, gfx_image(choice_icon[i]), cx + 26, cy + 14, 255);
        gfx_text(s, &ui_font_medium, cx + 112, cy + 26, choice_title[i], UI_TEXT, 255);
        gfx_text(s, &ui_font_regular, cx + 112, cy + 48, choice_text[i], UI_DIM, 255);
    }
    bottom_buttons(s, x, y, w, h, 0, "Continue", r->choice >= 0);
}

static void page_keyboard(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    title(s, x, y + 22, w, "Select Your Keyboard", "Choose the layout of the keyboard you are using.");
    static const char *const names[2] = { "U.S.", "Turkish Q" };
    static const char *const notes[2] = { "English (US)", "T\xC3\xBCrk\xC3\xA7" "e \xE2\x80\x94 \xC4\x9F \xC3\xBC \xC5\x9F \xC4\xB1 \xC3\xB6 \xC3\xA7" };
    for (int i = 0; i < 2; i++) {
        int cx = x + w / 2 - 250 + i * 260, cy = y + 92;
        gfx_rrect(s, cx, cy, 240, 54, 10, r->layout == i ? (ui_theme.dark ? 0x2F3A4F : 0xDCE6F8) : UI_CARD, 255);
        gfx_rrect_outline(s, cx, cy, 240, 54, 10, r->layout == i ? UI_ACCENT : UI_LINE, 255);
        gfx_circle(s, cx + 24, cy + 27, 8, r->layout == i ? UI_ACCENT : UI_LINE, 255);
        if (r->layout == i) gfx_circle(s, cx + 24, cy + 27, 3, 0xFFFFFF, 255);
        gfx_text(s, &ui_font_medium, cx + 44, cy + 9, names[i], UI_TEXT, 255);
        gfx_text(s, &ui_font_small, cx + 44, cy + 30, notes[i], UI_DIM, 255);
    }
    draw_keyboard(s, x + (w - 552) / 2, y + 180, r->layout, r->last_key);
    gfx_rrect(s, x + w / 2 - 200, y + 400, 400, 30, 7, UI_FIELD, 255);
    gfx_rrect_outline(s, x + w / 2 - 200, y + 400, 400, 30, 7, UI_ACCENT, 255);
    int tw = gfx_text(s, &ui_font_regular, x + w / 2 - 190, y + 406, r->test[0] ? r->test : "", UI_TEXT, 255);
    if (!r->test[0]) gfx_text(s, &ui_font_regular, x + w / 2 - 190, y + 406, "Type here to try your keyboard", UI_DIM, 255);
    else if ((uihost_ms() / 500) % 2 == 0) gfx_fill(s, x + w / 2 - 189 + tw, y + 406, 2, 18, UI_TEXT);
    bottom_buttons(s, x, y, w, h, 1, r->trying ? "Start" : "Continue", 1);
}

static void page_network(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    (void)r;
    title(s, x, y + 22, w, "Network", "tOS uses the wired network. It is set up by DHCP.");
    uihost_net_t n;
    uihost_net(&n);
    int cx = x + w / 2 - 260, cy = y + 100, cw = 520;
    gfx_rrect(s, cx, cy, cw, 230, 12, UI_CARD, 255);
    gfx_rrect_outline(s, cx, cy, cw, 230, 12, UI_LINE, 255);
    gfx_circle(s, cx + 40, cy + 40, 20, n.up && n.ip ? UI_GREEN : UI_DIM, 255);
    gfx_blit_tinted(s, gfx_image("glyph_ethernet-port"), cx + 32, cy + 32, 0xFFFFFF, 255);
    gfx_text(s, &ui_font_medium, cx + 72, cy + 22, "Ethernet", UI_TEXT, 255);
    gfx_text(s, &ui_font_regular, cx + 72, cy + 42, n.up && n.ip ? "Connected" : "Not connected", UI_DIM, 255);
    char v[4][32];
    ip_text(v[0], n.ip); ip_text(v[1], n.netmask); ip_text(v[2], n.gateway); ip_text(v[3], n.dns);
    static const char *const k[4] = { "IP Address", "Subnet Mask", "Router", "DNS Server" };
    for (int i = 0; i < 4; i++) {
        int ry = cy + 80 + i * 34;
        gfx_hline(s, cx + 16, ry - 6, cw - 32, UI_LINE, 255);
        gfx_text(s, &ui_font_regular, cx + 24, ry + 2, k[i], UI_TEXT, 255);
        const char *val = n.ip ? v[i] : "\xE2\x80\x94";
        gfx_text(s, &ui_font_regular, cx + cw - 24 - gfx_text_width(&ui_font_regular, val), ry + 2, val, UI_DIM, 255);
    }
    ui_button(s, x + w / 2 - 80, cy + 248, 160, 30, n.up && n.ip ? "Renew DHCP Lease" : "Connect", UI_CARD2, UI_TEXT, 0);
    bottom_buttons(s, x, y, w, h, 1, "Continue", 1);
}

static void page_account(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    title(s, x, y + 22, w, "Create a Computer Account", "This account is the computer's administrator.");
    static const char *const lab[F_COUNT] = { "Full name:", "Account name:", "Password:", "Verify:" };
    int fx = x + 300, fw = w - 340;
    for (int i = 0; i < F_COUNT; i++) field_box(s, fx, y + 100 + i * 44, fw, lab[i], r->field[i], i >= F_PASS, r->focus == i);
    /* the chosen picture, large, and all of them to pick from */
    char key[12] = "avatar_";
    key[7] = (char)('0' + r->picture / 10); key[8] = (char)('0' + r->picture % 10); key[9] = 0;
    gfx_blit(s, gfx_image(key), x + 120, y + 106, 255);
    gfx_text_center(s, &ui_font_small, x + 60, y + 208, 216, "Picture", UI_DIM, 255);
    for (int i = 0; i < 29; i++) {
        int px = x + 46 + (i % 15) * 52, py = y + 284 + (i / 15) * 56;
        key[7] = (char)('0' + i / 10); key[8] = (char)('0' + i % 10);
        if (i == r->picture) gfx_circle(s, px + 22, py + 22, 26, UI_ACCENT, 255);
        gfx_blit_scaled(s, gfx_image(key), px, py, 44, 44, 255);
    }
    if (r->err[0]) gfx_text_center(s, &ui_font_regular, x, y + h - 88, w, r->err, UI_RED, 255);
    bottom_buttons(s, x, y, w, h, 1, "Continue", r->field[F_USER][0] && r->field[F_PASS][0]);
}

static void page_disk(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    title(s, x, y + 22, w, "Select the Disk", "Choose the disk where you want to install tOS.");
    int shown = 0;
    for (int i = 0; i < r->nd; i++) {
        uihost_disk_t *d = &r->disks[i];
        int cx = x + w / 2 - (r->nd * 170) / 2 + shown * 170, cy = y + 110;
        if (r->disk == i) gfx_rrect(s, cx, cy, 150, 170, 12, ui_theme.dark ? 0x2F3A4F : 0xDCE6F8, 255);
        gfx_blit(s, gfx_image(d->removable ? "doc64_disk" : "doc64_hdd"), cx + 43, cy + 16, 255);
        gfx_text_center(s, &ui_font_medium, cx, cy + 92, 150, d->model, UI_TEXT, 255);
        char sz[24] = "";
        uint64_t b = d->size;
        ui_num(sz, (int64_t)(b / 1000000)); strcat(sz, " MB");
        gfx_text_center(s, &ui_font_small, cx, cy + 114, 150, sz, UI_DIM, 255);
        gfx_text_center(s, &ui_font_small, cx, cy + 132, 150, d->fs[0] ? d->fs : "Empty", UI_DIM, 255);
        if (r->disk == i) gfx_rrect_outline(s, cx, cy, 150, 170, 12, UI_ACCENT, 255);
        shown++;
    }
    if (!r->nd) gfx_text_center(s, &ui_font_regular, x, y + 180, w, "No disk tOS can be installed on was found.", UI_DIM, 255);
    else gfx_text_center(s, &ui_font_regular, x, y + 310, w, "Everything on the chosen disk will be erased.", UI_ORANGE, 255);
    bottom_buttons(s, x, y, w, h, 1, "Install", r->disk >= 0);
}

static void page_install(rc_t *r, gfx_surface_t *s, int x, int y, int w, int h)
{
    int done = !r->installing;
    title(s, x, y + 90, w, done ? (r->result == 0 ? "tOS is installed" : "The installation failed") : "Installing tOS",
          done ? (r->result == 0 ? "Restart the computer to start tOS from the disk." : r->status) : "This takes a moment. Don't turn off the computer.");
    gfx_blit(s, gfx_image("rec_install"), x + w / 2 - 32, y + 190, 255);
    int bx = x + w / 2 - 180, by = y + 300;
    gfx_rrect(s, bx, by, 360, 8, 4, UI_LINE, 255);
    int fw = 360 * (done && r->result == 0 ? 100 : r->pct) / 100;
    if (fw > 8) gfx_rrect(s, bx, by, fw, 8, 4, UI_ACCENT, 255);
    if (!done) gfx_text_center(s, &ui_font_small, x, by + 22, w, r->status, UI_DIM, 255);
    if (done) bottom_buttons(s, x, y, w, h, r->result != 0, r->result == 0 ? "Restart" : 0, 1);
}

static void rc_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    rc_t *r = a->st;
    strcpy(a->title, "tOS Recovery");
    switch (r->page) {
    case P_HOME: page_home(r, s, x, y, w, h); break;
    case P_KEYBOARD: page_keyboard(r, s, x, y, w, h); break;
    case P_NETWORK: page_network(r, s, x, y, w, h); break;
    case P_ACCOUNT: page_account(r, s, x, y, w, h); break;
    case P_DISK: page_disk(r, s, x, y, w, h); break;
    case P_INSTALL: page_install(r, s, x, y, w, h); break;
    }
}

/* ------------------------------------------------------------------ input */

static void load_disks(rc_t *r)
{
    uihost_disk_t all[8];
    int n = uihost_disks(all, 8);
    r->nd = 0;
    /* tOS installs on an ATA disk that is not the one it is running from */
    for (int i = 0; i < n && r->nd < 8; i++)
        if (!all[i].removable && !all[i].startup && !strncmp(all[i].dev, "ata", 3)) r->disks[r->nd++] = all[i];
    r->disk = r->nd ? 0 : -1;
}

static int account_ok(rc_t *r)
{
    const char *u = r->field[F_USER];
    if (!u[0] || u[0] < 'a' || u[0] > 'z') { strcpy(r->err, "The account name must start with a lowercase letter."); return 0; }
    for (const char *c = u; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) { strcpy(r->err, "Use only lowercase letters, digits, \"-\" and \"_\" in the account name."); return 0; }
    if (!r->field[F_PASS][0]) { strcpy(r->err, "Choose a password."); return 0; }
    if (strcmp(r->field[F_PASS], r->field[F_VERIFY])) { strcpy(r->err, "The passwords don't match."); return 0; }
    r->err[0] = 0;
    return 1;
}

static void start_install(rc_t *r)
{
    char host[40];
    strcpy(host, r->field[F_USER]);
    strcat(host, "-tos");
    r->page = P_INSTALL;
    r->pct = 0;
    strcpy(r->status, "Starting\xE2\x80\xA6");
    if (uihost_install_start(r->disks[r->disk].dev, host, r->field[F_USER], r->field[F_FULL], r->field[F_PASS], r->layout, r->picture) == 0) r->installing = 1;
    else { r->installing = 0; r->result = -1; strcpy(r->status, "The installation could not start."); }
}

static void rc_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    rc_t *r = a->st;
    a->dirty = 1;
    int w = a->w;
    switch (r->page) {
    case P_HOME:
        for (int i = 0; i < 3; i++) if (ui_hit(px, py, 60, HOME_ROW_Y(i), w - 120, 92)) r->choice = i;
        if (hit_next(a, px, py) && r->choice >= 0) {
            if (r->choice == 1) uihost_open("diskutil", 0);
            else { r->trying = r->choice == 2; r->page = P_KEYBOARD; }
        }
        break;
    case P_KEYBOARD:
        for (int i = 0; i < 2; i++) if (ui_hit(px, py, w / 2 - 250 + i * 260, 92, 240, 54)) { r->layout = i; uihost_keyboard_set(i); }
        if (hit_back(a, px, py)) r->page = P_HOME;
        if (hit_next(a, px, py)) { if (r->trying) uihost_try_live(); else r->page = P_NETWORK; }
        break;
    case P_NETWORK:
        if (ui_hit(px, py, w / 2 - 80, 348, 160, 30)) uihost_net_start();
        if (hit_back(a, px, py)) r->page = P_KEYBOARD;
        if (hit_next(a, px, py)) r->page = P_ACCOUNT;
        break;
    case P_ACCOUNT:
        for (int i = 0; i < F_COUNT; i++) if (ui_hit(px, py, 450, 100 + i * 44, w - 490, 30)) r->focus = i;
        for (int i = 0; i < 29; i++) if (ui_hit(px, py, 46 + (i % 15) * 52, 284 + (i / 15) * 56, 44, 44)) r->picture = i;
        if (hit_back(a, px, py)) r->page = P_NETWORK;
        if (hit_next(a, px, py) && account_ok(r)) { load_disks(r); r->page = P_DISK; }
        break;
    case P_DISK:
        for (int i = 0; i < r->nd; i++) if (ui_hit(px, py, w / 2 - (r->nd * 170) / 2 + i * 170, 110, 150, 170)) r->disk = i;
        if (hit_back(a, px, py)) r->page = P_ACCOUNT;
        if (hit_next(a, px, py) && r->disk >= 0) start_install(r);
        break;
    case P_INSTALL:
        if (r->installing) break;
        if (r->result == 0 && hit_next(a, px, py)) uihost_restart();
        if (r->result != 0 && hit_back(a, px, py)) r->page = P_DISK;
        break;
    }
}

static void rc_key(uiapp_t *a, int k)
{
    rc_t *r = a->st;
    a->dirty = 1;
    if (r->page == P_KEYBOARD) {
        r->last_key = k >= 'A' && k <= 'Z' ? k + 32 : k;
        if (k == '\b') text_backspace(r->test);
        else text_append(r->test, 40, k);
        return;
    }
    if (r->page == P_ACCOUNT) {
        char *f = r->field[r->focus];
        if (k == '\t' || k == '\n') { r->focus = (r->focus + 1) % F_COUNT; return; }
        if (k == '\b') text_backspace(f);
        else if (r->focus == F_FULL) text_append(f, 44, k);                  /* the full name may be Turkish */
        else if (k >= 32 && k < 127 && (int)strlen(f) < 40) { int n = (int)strlen(f); f[n] = (char)k; f[n + 1] = 0; }
        else return;
        if (r->focus == F_USER) r->user_edited = 1;
        if (r->focus == F_FULL && !r->user_edited) {
            /* the account name follows the full name: lowercase, no spaces */
            int o = 0;
            for (const unsigned char *c = (const unsigned char *)f; *c && o < 20; c++) {
                unsigned cp = *c;
                if ((cp & 0xE0) == 0xC0 && c[1]) { cp = ((cp & 0x1F) << 6) | (c[1] & 0x3F); c++; }
                /* Turkish letters become their plain Latin ones, as the Mac does: Demır -> demir */
                switch (cp) {
                case 0x131: case 0x130: case 0xCC: case 0xCD: case 0xEC: case 0xED: cp = 'i'; break;
                case 0x11E: case 0x11F: cp = 'g'; break;
                case 0xDC: case 0xFC: cp = 'u'; break;
                case 0x15E: case 0x15F: cp = 's'; break;
                case 0xD6: case 0xF6: cp = 'o'; break;
                case 0xC7: case 0xE7: cp = 'c'; break;
                }
                char ch = cp >= 'A' && cp <= 'Z' ? (char)(cp + 32) : (char)cp;
                if (cp < 128 && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) r->field[F_USER][o++] = ch;
            }
            r->field[F_USER][o] = 0;
        }
    }
}

static int rc_tick(uiapp_t *a)
{
    rc_t *r = a->st;
    if (r->installing) {
        int rc = uihost_install_poll(&r->pct, r->status, sizeof(r->status));
        if (rc != -2) { r->installing = 0; r->result = rc; }
        return 1;
    }
    static uint32_t blink;
    uint32_t t = uihost_ms() / 500;
    if ((r->page == P_ACCOUNT || r->page == P_KEYBOARD || r->page == P_NETWORK) && t != blink) { blink = t; return 1; }
    return 0;
}

static void rc_init(uiapp_t *a, const char *arg)
{
    (void)arg;
    rc_t *r = a->st;
    r->choice = 0;
    r->picture = 0;
    r->layout = uihost_keyboard();
}

const uiapp_class_t app_recovery = {
    "recovery", "tOS Recovery", "launcher", 860, 560, 0, sizeof(rc_t),
    rc_init, rc_draw, rc_press, 0, 0, rc_key, 0, rc_tick, 0,
    0, 0, 0,
};
