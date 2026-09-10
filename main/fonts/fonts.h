// main/fonts/fonts.h —— 应用自带字体的声明。
// font_cjk_20:20px/1bpp,ASCII + GB2312 一级汉字 + 中文标点,用于姓名等用户内容。
// font_cjk_16:同字符集的 16px 版本,用于提示栏、设置项等界面框架文字。
// 两者都把缺失字形回退到同尺寸的 Montserrat,因此 LV_SYMBOL_* 图标可以和中文混排。
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(font_cjk_20);
LV_FONT_DECLARE(font_cjk_16);
