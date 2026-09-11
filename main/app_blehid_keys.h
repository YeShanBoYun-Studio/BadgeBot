// main/app_blehid_keys.h —— 翻页器键位映射的纯逻辑(不依赖 ESP-IDF,主机可测)。
// 方向键语义:把"上一页/下一页"翻译成 USB HID 键盘用途码(Usage ID)。
#pragma once

#include <stdint.h>

// 键位模式:0 = PgUp/PgDn(演示文稿、PDF),1 = 左/右方向键(漫画、图片查看器)
#define APP_BLEHID_MAP_PAGE   0
#define APP_BLEHID_MAP_ARROWS 1

// dir: -1 = 上一页, +1 = 下一页;返回 USB HID Usage ID(keyboard page)
uint8_t app_blehid_keycode(uint8_t map, int dir);
