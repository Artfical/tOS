#ifndef HID_H
#define HID_H

#define HID_BOOT_PROTOCOL 0
#define HID_REPORT_PROTOCOL 1

#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_SET_REPORT 0x09

#define HID_REPORT_TYPE_INPUT 1
#define HID_REPORT_TYPE_OUTPUT 2
#define HID_REPORT_TYPE_FEATURE 3

#define USB_KEY_NONE 0
#define USB_KEY_ROLLOVER 1
#define USB_KEY_A 4
#define USB_KEY_B 5
#define USB_KEY_C 6
#define USB_KEY_D 7
#define USB_KEY_E 8
#define USB_KEY_F 9
#define USB_KEY_G 10
#define USB_KEY_H 11
#define USB_KEY_I 12
#define USB_KEY_J 13
#define USB_KEY_K 14
#define USB_KEY_L 15
#define USB_KEY_M 16
#define USB_KEY_N 17
#define USB_KEY_O 18
#define USB_KEY_P 19
#define USB_KEY_Q 20
#define USB_KEY_R 21
#define USB_KEY_S 22
#define USB_KEY_T 23
#define USB_KEY_U 24
#define USB_KEY_V 25
#define USB_KEY_W 26
#define USB_KEY_X 27
#define USB_KEY_Y 28
#define USB_KEY_Z 29
#define USB_KEY_1 30
#define USB_KEY_2 31
#define USB_KEY_3 32
#define USB_KEY_4 33
#define USB_KEY_5 34
#define USB_KEY_6 35
#define USB_KEY_7 36
#define USB_KEY_8 37
#define USB_KEY_9 38
#define USB_KEY_0 39
#define USB_KEY_ENTER 40
#define USB_KEY_ESC 41
#define USB_KEY_BACKSPACE 42
#define USB_KEY_TAB 43
#define USB_KEY_SPACE 44
#define USB_KEY_MINUS 45
#define USB_KEY_EQUAL 46
#define USB_KEY_LEFTBRACE 47
#define USB_KEY_RIGHTBRACE 48
#define USB_KEY_BACKSLASH 49
#define USB_KEY_SEMICOLON 51
#define USB_KEY_QUOTE 52
#define USB_KEY_GRAVE 53
#define USB_KEY_COMMA 54
#define USB_KEY_DOT 55
#define USB_KEY_SLASH 56
#define USB_KEY_CAPSLOCK 57
#define USB_KEY_F1 58
#define USB_KEY_F2 59
#define USB_KEY_F3 60
#define USB_KEY_F4 61
#define USB_KEY_F5 62
#define USB_KEY_F6 63
#define USB_KEY_F7 64
#define USB_KEY_F8 65
#define USB_KEY_F9 66
#define USB_KEY_F10 67
#define USB_KEY_F11 68
#define USB_KEY_F12 69
#define USB_KEY_DELETE 76

#define USB_MOD_LCTRL 0x01
#define USB_MOD_LSHIFT 0x02
#define USB_MOD_LALT 0x04
#define USB_MOD_LGUI 0x08
#define USB_MOD_RCTRL 0x10
#define USB_MOD_RSHIFT 0x20
#define USB_MOD_RALT 0x40
#define USB_MOD_RGUI 0x80

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} __attribute__((packed)) hid_keyboard_report_t;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) hid_mouse_report_t;

#define HID_MOUSE_LEFT   1
#define HID_MOUSE_RIGHT  2
#define HID_MOUSE_MIDDLE 4

char usb_keycode_to_ascii(uint8_t keycode, uint8_t modifiers);

#endif
