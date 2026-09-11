// main/app_blehid_keys.c —— 键位映射查表;见头文件。
#include "app_blehid_keys.h"

// USB HID Keyboard/Keypad Page(0x07)用途码
#define HID_KEY_PAGE_UP    0x4B
#define HID_KEY_PAGE_DOWN  0x4E
#define HID_KEY_LEFT_ARROW 0x50
#define HID_KEY_RIGHT_ARROW 0x4F

uint8_t app_blehid_keycode(uint8_t map, int dir)
{
    if (map == APP_BLEHID_MAP_ARROWS) {
        return (dir < 0) ? HID_KEY_LEFT_ARROW : HID_KEY_RIGHT_ARROW;
    }
    return (dir < 0) ? HID_KEY_PAGE_UP : HID_KEY_PAGE_DOWN;
}
