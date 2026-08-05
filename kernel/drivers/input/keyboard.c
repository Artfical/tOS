#include "keyboard.h"
#include "usb_keyboard.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "terminal.h"
#include "gui.h"
#include "wm.h"
#include "net.h"
#include "scheduler.h"

static volatile char key_buffer[256];
static volatile int key_buffer_head = 0;
static volatile int key_buffer_tail = 0;
static volatile int special_buf[16];
static volatile int special_head = 0;
static volatile int special_tail = 0;

/* Raw press/release event queue: everything above (key_buffer,
 * special_buf) only ever tracks key *presses* -- fine for line-
 * editing/menus, but games (DOOM's platform layer wants both a press
 * and a matching release per key, e.g. to know when to stop moving)
 * need real key-up events too, which nothing here previously
 * recorded at all (`if (scancode & 0x80) return;` silently dropped
 * every break code). This queue is populated for every event
 * regardless of what else consumes it, so it doesn't disturb any
 * existing caller. */
typedef struct { uint8_t key; uint8_t pressed; } kbd_raw_event_t;
static volatile kbd_raw_event_t raw_queue[64];
static volatile int raw_head = 0;
static volatile int raw_tail = 0;

static void push_raw_event(uint8_t key, int pressed)
{
    int next = (raw_head + 1) % 64;
    if (next != raw_tail) {
        raw_queue[raw_head].key = key;
        raw_queue[raw_head].pressed = (uint8_t)pressed;
        raw_head = next;
    }
}

int keyboard_get_raw_event(uint8_t *key, int *pressed)
{
    /* Same focus gate every other input accessor here uses (see
     * keyboard_get_special()) -- without it, a windowed app using
     * this queue (e.g. DOOM opened as a desktop window rather than
     * the CLI `doom` command's fullscreen takeover) would keep
     * draining key events even while some other window has focus. */
    if (!wm_current_task_has_focus()) return 0;
    if (raw_head == raw_tail) return 0;
    *key = raw_queue[raw_tail].key;
    *pressed = raw_queue[raw_tail].pressed;
    raw_tail = (raw_tail + 1) % 64;
    return 1;
}

static int caps_lock = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int extended = 0;
volatile int interrupt_char = -1;
void (*interrupt_callback)(void) = NULL;
static int keyboard_layout = KBD_LAYOUT_US;

static const char scancode_lower_us[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_upper_us[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_lower_trq[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '*', '-', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 0xFD, 'o', 'p', 0xF0, 0xFC, '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xFE, 'i', ',',
    0, ';', 'z', 'x', 'c', 'v', 'b', 'n', 'm', 0xF6, 0xE7, '.', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char scancode_upper_trq[] = {
    0, 0, '!', '"', '^', '+', '%', '&', '/', '(', ')', '=', '?', '_', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xD0, 0xDC, '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xDE, 0xDD, ';',
    0, ':', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 0xD6, 0xC7, '.', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};

static const char *scancode_lower;
static const char *scancode_upper;

void keyboard_set_layout(int layout)
{
    keyboard_layout = layout;
    if (layout == KBD_LAYOUT_TRQ) {
        scancode_lower = scancode_lower_trq;
        scancode_upper = scancode_upper_trq;
    } else {
        scancode_lower = scancode_lower_us;
        scancode_upper = scancode_upper_us;
    }
}

int keyboard_get_layout(void)
{
    return keyboard_layout;
}

void keyboard_push_char(char c)
{
    int next = (key_buffer_head + 1) % 256;
    if (next != key_buffer_tail) {
        key_buffer[key_buffer_head] = c;
        key_buffer_head = next;
    }
}

static void push_special(uint8_t scancode)
{
    int next = (special_head + 1) % 16;
    if (next != special_tail) {
        if (scancode == 0x4B) special_buf[special_head] = 1;
        else if (scancode == 0x4D) special_buf[special_head] = 2;
        else if (scancode == 0x48) special_buf[special_head] = 3;
        else if (scancode == 0x50) special_buf[special_head] = 4;
        special_head = next;
    }
}

/* Raw-event key codes for non-ASCII keys (arrows), matching
 * push_special()'s existing 1=left/2=right/3=up/4=down convention so
 * both queues agree on what these numbers mean. */
#define RAWKEY_LEFT  1
#define RAWKEY_RIGHT 2
#define RAWKEY_UP    3
#define RAWKEY_DOWN  4

static void process_scancode(uint8_t scancode)
{
    if (scancode == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        if (scancode == 0x53) { keyboard_push_char(127); return; }
        uint8_t base = scancode & 0x7F;
        int is_release = (scancode & 0x80) != 0;
        int rawkey = 0;
        if (base == 0x4B) rawkey = RAWKEY_LEFT;
        else if (base == 0x4D) rawkey = RAWKEY_RIGHT;
        else if (base == 0x48) rawkey = RAWKEY_UP;
        else if (base == 0x50) rawkey = RAWKEY_DOWN;
        if (rawkey) push_raw_event((uint8_t)rawkey, !is_release);
        if (!is_release && (base == 0x48 || base == 0x50 || base == 0x4B || base == 0x4D)) {
            push_special(base);
        }
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }
    if (scancode == 0x1D) { ctrl_pressed = 1; return; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return; }
    if (scancode == 0x3A) { caps_lock = !caps_lock; return; }

    /* Raw press/release for every plain key, independent of the
     * shift/caps/ctrl-aware ASCII translation below -- a game wants
     * to know "the physical key bound to move-forward went up", not
     * "an uppercase W was typed". Uses the unshifted lowercase table
     * purely as a stable per-scancode identifier. */
    {
        uint8_t base = scancode & 0x7F;
        int is_release = (scancode & 0x80) != 0;
        uint8_t rawkey = 0;
        if (base == 0x01) rawkey = 27; /* Escape isn't in scancode_lower */
        else if (base < 0x53) rawkey = (uint8_t)scancode_lower[base];
        if (rawkey) push_raw_event(rawkey, !is_release);
    }

    if (scancode & 0x80) return;

    if (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D) {
        push_special(scancode);
        return;
    }

    char key = 0;
    if (scancode < 0x53) {
        if (shift_pressed)
            key = scancode_upper[scancode];
        else
            key = scancode_lower[scancode];

        if (key >= 'a' && key <= 'z' && caps_lock && !ctrl_pressed) key -= 32;
        else if (key >= 'A' && key <= 'Z' && caps_lock && !ctrl_pressed) key += 32;
        if (ctrl_pressed && key >= 'a' && key <= 'z') key -= 0x60;
        else if (ctrl_pressed && key >= 'A' && key <= 'Z') key -= 0x40;
    }
    if (key) {
        keyboard_push_char(key);
        if (interrupt_char >= 0 && key == (char)interrupt_char && interrupt_callback)
            interrupt_callback();
    }
}

/* Both of these used to unconditionally `sti` at the end -- fine for
 * keyboard_callback() (only ever reached with interrupts already on,
 * since it's an IRQ handler), but keyboard_poll() is also called from
 * keyboard_getchar(), which is reachable from a ring3 .t program's
 * blocking tos_read() (SYS_READ, entered via the int $0x80 interrupt
 * gate -- which auto-clears IF for the whole syscall so it can't be
 * preempted mid-handling). An unconditional `sti` here silently turned
 * interrupts back on in the middle of that syscall, letting the 100Hz
 * PIT timer (int $32) preempt right there -- a genuine nested hardware
 * interrupt, no explicit task_yield() call needed to trigger it. That
 * caused a real, reproducible GPF (corrupted ring3 register/stack state
 * on return) the moment a user typed into ssh.t's blocking hostname
 * prompt. Save/restore the actual incoming IF bit with pushfl/popfl
 * instead, so a caller that entered with interrupts already off (a
 * syscall handler) leaves with them off too. */
static void keyboard_callback(registers_t *regs)
{
    (void)regs;
    uint32_t flags;
    asm volatile("pushfl; pop %0; cli" : "=r"(flags));
    uint8_t status;
    while (((status = inb(0x64)) & 1) && !(status & 0x20))
        process_scancode(inb(0x60));
    if (flags & 0x200) asm volatile("sti");
}

static void keyboard_poll(void)
{
    uint32_t flags;
    asm volatile("pushfl; pop %0; cli" : "=r"(flags));
    uint8_t status = inb(0x64);
    if ((status & 1) && !(status & 0x20)) process_scancode(inb(0x60));
    if (flags & 0x200) asm volatile("sti");
}

int keyboard_data_available(void)
{
    return key_buffer_head != key_buffer_tail;
}

char keyboard_getchar(void)
{
    for (;;) {
        if (wm_current_task_has_focus() && key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % 256;
            return c;
        }
        if (!wm_current_task_has_focus()) {
            /* Used to call task_yield() (int $32) here -- but this
             * function is reachable from a ring3 .t program's
             * blocking tos_read() (SYS_READ, entered via int $0x80),
             * and firing a second software interrupt while already
             * inside that trap gate's handler is the same nested-
             * interrupt reentrancy class of bug previously suspected
             * (never confirmed) in tcp_connect()/arp_resolve()'s own
             * yielding retry loops -- confirmed here via a reproducible
             * GPF: a .t program's ring3 write right after tos_read()
             * returned faulted with a corrupted pointer, consistent
             * with clobbered register/stack state from a task switch
             * nested inside the syscall handler. Poll directly instead
             * of yielding -- still lets an unfocused window's task
             * back off from actually consuming input it won't get,
             * without re-entering the scheduler mid-syscall. */
            keyboard_poll();
            net_poll();
            continue;
        }
        keyboard_poll();
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
            net_poll();
            asm volatile("hlt");
        }
    }
}

void keyboard_readline(char *buf, int max)
{
    int i = 0;
    for (;;) {
        gui_poll();
        char c = keyboard_getchar();
        if (c == '\n') {
            terminal_putchar('\n');
            buf[i] = '\0';
            return;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 127) {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 3) {
            terminal_putchar('^');
            terminal_putchar('C');
            terminal_putchar('\n');
            buf[0] = '\0';
            return;
        } else if ((unsigned char)c >= ' ' && i < max - 1) {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
}

int keyboard_get_special(void)
{
    if (wm_current_task_has_focus() && special_head != special_tail) {
        int k = special_buf[special_tail];
        special_tail = (special_tail + 1) % 16;
        return k;
    }
    return 0;
}

int keyboard_shift_held(void) { return shift_pressed; }
int keyboard_ctrl_held(void) { return ctrl_pressed; }

int keyboard_yesno(void)
{
    for (;;) {
        gui_poll();
        keyboard_poll();
        if (special_head != special_tail) {
            int k = special_buf[special_tail];
            special_tail = (special_tail + 1) % 16;
            if (k == 1) return 0;
            if (k == 2) return 1;
        }
        if (key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % 256;
            if (c == 'y' || c == 'Y') return 1;
            if (c == 'n' || c == 'N') return 0;
            if (c == '\n') return 2;
        }
        char usb_c;
        if (usb_keyboard_read(&usb_c)) {
            keyboard_push_char(usb_c);
        } else {
            net_poll();
            asm volatile("hlt");
        }
    }
}

void keyboard_init(void)
{
    keyboard_set_layout(KBD_LAYOUT_US);
    isr_register_handler(33, keyboard_callback);
    irq_ack(1);
    usb_keyboard_init();
}
