#pragma once

#include "lvgl.h"

// 旧版固定配色,仍被早期 demo 页引用;新代码请用 ui_pixel_theme() 的语义角色。
#define UI_SKY        0x1689E8
#define UI_SKY_DARK   0x0872C9
#define UI_SKY_DIM    0x7FB8EE
#define UI_INK        0x17202A
#define UI_PAPER      0xF4F4EA
#define UI_GRASS      0x82BE2D
#define UI_GRASS_DARK 0x55951D
#define UI_YELLOW     0xFFD928
#define UI_ORANGE     0xFFB23E
#define UI_RED        0xE43B2F
#define UI_MUTED      0xD9E7EC

#define UI_THEME_COUNT 2        // 0 = 浅色,1 = 深色

// 语义配色角色:页面和 ui_pixel 内部都只引用角色,不写死颜色,
// 换主题/加主题时只需要在 ui_pixel.c 里增加一份调色板。
typedef struct {
    const char *name;
    uint32_t bg;         // 屏幕底色
    uint32_t panel;      // 面板/卡片底色
    uint32_t ink;        // 主要文字与描边
    uint32_t muted;      // 次要文字
    uint32_t dim;        // 未激活图标
    uint32_t plate;      // 标题牌底色
    uint32_t accent;     // 选中高亮
    uint32_t footer;     // 底部条
    uint32_t footer_hi;  // 底部条顶部高光线
    uint32_t chip;       // 提示条按键帽底色
    uint32_t chip_text;  // 按键帽内的图标/文字颜色(必须与 chip 强对比)
} ui_theme_t;

const ui_theme_t *ui_pixel_theme(void);
void ui_pixel_set_theme(uint8_t index);   // 越界取浅色;对已建屏不生效,新屏生效

// 建一个带标题牌、顶部状态栏与底部条的屏。状态栏内容由 ui_pixel_status_update() 统一刷新。
lv_obj_t *ui_pixel_screen_create(const char *title);
// 刷新当前屏右上角的状态栏:电量(-1 = 未知)、音量、Wi-Fi 是否已连接、蓝牙是否已连接。
// 值会缓存,之后新建的屏直接沿用;由 main.c 定时调用,页面无需关心。
void ui_pixel_status_update(int soc, uint8_t volume, bool wifi, bool ble);

// 底部按键提示条的一个条目。按键帽用边框区分按法:粗边 = 长按,细边 = 短按。
// keys 可以是 LV_SYMBOL_* 图标、"OK" 等短文本,经 font_cjk_16 渲染(图标走 Montserrat 回退)。
typedef struct {
    const char *keys;
    const char *action;
    bool        long_press;
} ui_hint_t;
void ui_pixel_hints(lv_obj_t *scr, const ui_hint_t *hints, int count);

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
// 删除承载吉祥物的屏幕之前必须调用:眨眼是无限循环动画,对象释放后动画回调会写已释放内存。
void ui_pixel_mascot_stop(lv_obj_t *mascot);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
