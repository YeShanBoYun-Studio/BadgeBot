#include "ui_pixel.h"
#include "fonts/fonts.h"
#include <string.h>

static void start_blink(lv_obj_t *eye);

/* ---------------- 主题 ----------------
 * 页面与 ui_pixel 内部只引用语义角色;新增主题 = 在 THEMES[] 里加一份调色板。 */
static const ui_theme_t THEMES[UI_THEME_COUNT] = {
    [0] = { "LIGHT",
            .bg = UI_SKY,     .panel = UI_PAPER,    .ink = UI_INK,
            .muted = 0x4A5A66, .dim = UI_SKY_DIM,  .plate = UI_PAPER,
            .accent = UI_YELLOW, .footer = UI_GRASS, .footer_hi = 0xA7D93E,
            .chip = UI_INK,   .chip_text = UI_PAPER },
    [1] = { "DARK",
            .bg = 0x0F141B,   .panel = 0x1C242E,    .ink = 0xE9EEF4,
            .muted = 0x93A3B0, .dim = 0x36414D,    .plate = 0x232D39,
            .accent = UI_YELLOW, .footer = 0x131A22, .footer_hi = 0x27313D,
            .chip = 0x2A3542, .chip_text = 0xE9EEF4 },
};
static uint8_t s_theme;

const ui_theme_t *ui_pixel_theme(void) { return &THEMES[s_theme]; }

void ui_pixel_set_theme(uint8_t index)
{
    s_theme = index < UI_THEME_COUNT ? index : 0;
}

/* ---------------- 基础绘制 ---------------- */

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* ---------------- 状态栏 ---------------- */

/* 状态栏控件属于当前屏;屏删除时由 LV_EVENT_DELETE 回调清空,避免悬空指针。
 * 最近一次的状态值缓存下来,新屏建好后立即回填,不用等下一次刷新。 */
static lv_obj_t *s_st_wifi, *s_st_ble, *s_st_vol, *s_st_batt;
static int     s_last_soc = -1;
static uint8_t s_last_vol;
static bool    s_last_wifi, s_last_ble;

static void on_screen_delete(lv_event_t *e)
{
    (void)e;
    s_st_wifi = s_st_ble = s_st_vol = s_st_batt = NULL;
}

static void status_apply(void)
{
    if (!s_st_batt) return;
    const ui_theme_t *th = ui_pixel_theme();

    const char *batt = LV_SYMBOL_BATTERY_EMPTY;
    if      (s_last_soc >= 90) batt = LV_SYMBOL_BATTERY_FULL;
    else if (s_last_soc >= 60) batt = LV_SYMBOL_BATTERY_3;
    else if (s_last_soc >= 35) batt = LV_SYMBOL_BATTERY_2;
    else if (s_last_soc >= 10) batt = LV_SYMBOL_BATTERY_1;
    if (s_last_soc < 0) lv_label_set_text_fmt(s_st_batt, "%s --%%", batt);
    else                lv_label_set_text_fmt(s_st_batt, "%s %d%%", batt, s_last_soc);
    lv_obj_set_style_text_color(s_st_batt,
        lv_color_hex(s_last_soc >= 0 && s_last_soc <= 20 ? UI_RED : th->ink), 0);

    lv_label_set_text(s_st_vol, s_last_vol == 0 ? LV_SYMBOL_MUTE
                              : s_last_vol < 50 ? LV_SYMBOL_VOLUME_MID
                                                : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_st_vol, lv_color_hex(th->ink), 0);
    lv_obj_set_style_text_color(s_st_wifi, lv_color_hex(s_last_wifi ? th->ink : th->dim), 0);
    lv_obj_set_style_text_color(s_st_ble, lv_color_hex(th->ink), 0);
    if (s_last_ble) lv_obj_remove_flag(s_st_ble, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_st_ble, LV_OBJ_FLAG_HIDDEN);
}

void ui_pixel_status_update(int soc, uint8_t volume, bool wifi, bool ble)
{
    s_last_soc = soc;
    s_last_vol = volume;
    s_last_wifi = wifi;
    s_last_ble = ble;
    status_apply();
}

static void add_status_bar(lv_obj_t *scr)
{
    /* 右上角两行:第一行 Wi-Fi / 蓝牙 / 音量图标,第二行电池图标 + 百分比。 */
    s_st_wifi = ui_pixel_label(scr, LV_SYMBOL_WIFI, &lv_font_montserrat_16, UI_PAPER);
    lv_obj_set_pos(s_st_wifi, 168, 9);
    s_st_ble = ui_pixel_label(scr, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_16, UI_PAPER);
    lv_obj_set_pos(s_st_ble, 193, 9);
    s_st_vol = ui_pixel_label(scr, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_16, UI_PAPER);
    lv_obj_set_pos(s_st_vol, 214, 9);
    s_st_batt = ui_pixel_label(scr, "", &lv_font_montserrat_16, UI_PAPER);
    lv_obj_set_pos(s_st_batt, 162, 27);
    lv_obj_set_width(s_st_batt, 74);
    lv_obj_set_style_text_align(s_st_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, NULL);
    status_apply();
}

/* ---------------- 屏幕骨架 ---------------- */

lv_obj_t *ui_pixel_screen_create(const char *title)
{
    const ui_theme_t *th = ui_pixel_theme();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(th->bg), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    block(scr, 0, 290, 240, 30, th->footer);
    block(scr, 0, 290, 240, 2, th->footer_hi);

    block(scr, 9, 12, 151, 33, th->ink);
    lv_obj_t *plate = block(scr, 5, 8, 151, 33, th->plate);
    lv_obj_set_style_border_color(plate, lv_color_hex(th->ink), 0);
    lv_obj_set_style_border_width(plate, 3, 0);
    lv_obj_t *heading = ui_pixel_label(plate, title, &lv_font_montserrat_20, th->ink);
    lv_obj_center(heading);

    add_status_bar(scr);
    return scr;
}

/* ---------------- 按键提示条 ----------------
 * 按键帽粗边 = 长按,细边 = 短按;动作文字用 font_cjk_16(中文,图标走 Montserrat 回退)。 */

// 粗估一段 UTF-8 文本在 font_cjk_16 下的像素宽:CJK/全角按 16px,其余按 10px。
static int text_width_16(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        w += (len >= 3) ? 16 : 10;
        s += len - 1;
    }
    return w;
}

void ui_pixel_hints(lv_obj_t *scr, const ui_hint_t *hints, int count)
{
    const ui_theme_t *th = ui_pixel_theme();
    // 先量总宽,让整行在 240px 内居中。按键帽用 Montserrat(LV_SYMBOL_* 图标原生渲染,
    // 不走 CJK 字体回退——回退在部分字形上不可靠);动作文字才用 font_cjk_16。
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += text_width_16(hints[i].keys) + 12;      // 按键帽
        total += text_width_16(hints[i].action) + 4;     // 动作文字
        total += 14;                                     // 条目间距
    }
    int x = (240 - (total - 14)) / 2;
    if (x < 4) x = 4;

    for (int i = 0; i < count; i++) {
        int key_w = text_width_16(hints[i].keys) + 12;
        lv_obj_t *cap = block(scr, x, 294, key_w, 22, th->chip);
        lv_obj_set_style_border_color(cap, lv_color_hex(th->ink), 0);
        lv_obj_set_style_border_width(cap, hints[i].long_press ? 3 : 1, 0);
        lv_obj_t *key_label = ui_pixel_label(cap, hints[i].keys, &lv_font_montserrat_16, th->chip_text);
        lv_obj_center(key_label);
        x += key_w + 4;
        lv_obj_t *act = ui_pixel_label(scr, hints[i].action, &font_cjk_16, th->ink);
        lv_obj_set_pos(act, x, 296);
        x += text_width_16(hints[i].action) + 14;
    }
}

/* ---------------- 面板 / 吉祥物 ---------------- */

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    block(parent, x + 5, y + 6, w, h, ui_pixel_theme()->ink);
    lv_obj_t *panel = block(parent, x, y, w, h, color);
    lv_obj_set_style_border_color(panel, lv_color_hex(ui_pixel_theme()->ink), 0);
    lv_obj_set_style_border_width(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    return panel;
}

lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 38, 48);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);

    /* 原创“小电视机器人”：天线、发光屏幕脸、橙色围巾与履带脚。 */
    block(m, 18, 0, 3, 6, UI_INK);
    block(m, 16, 0, 7, 3, UI_ORANGE);
    block(m, 3, 6, 32, 24, UI_INK);
    block(m, 0, 12, 5, 10, 0x7557D9);
    block(m, 33, 12, 5, 10, 0x7557D9);
    block(m, 7, 10, 24, 16, 0xB9F3FF);
    lv_obj_t *left_eye = block(m, 11, 14, 4, 6, 0x294B7A);
    lv_obj_t *right_eye = block(m, 23, 14, 4, 6, 0x294B7A);
    block(m, 16, 22, 7, 2, 0x7557D9);
    block(m, 10, 29, 18, 4, UI_ORANGE);
    block(m, 8, 33, 22, 11, 0x7557D9);
    block(m, 3, 35, 5, 7, 0xB9F3FF);
    block(m, 30, 35, 5, 7, 0xB9F3FF);
    block(m, 8, 44, 9, 4, UI_INK);
    block(m, 21, 44, 9, 4, UI_INK);
    start_blink(left_eye);
    start_blink(right_eye);
    return m;
}

static void jump_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void blink_eye(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void start_blink(lv_obj_t *eye)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, blink_eye);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_delay(&anim, 1700);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_mascot_jump(lv_obj_t *mascot)
{
    if (!mascot) return;
    int y = lv_obj_get_y(mascot);
    lv_anim_delete(mascot, jump_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, mascot);
    lv_anim_set_exec_cb(&anim, jump_y);
    lv_anim_set_values(&anim, y, y - 5);
    lv_anim_set_duration(&anim, 110);
    lv_anim_set_playback_duration(&anim, 140);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

void ui_pixel_mascot_stop(lv_obj_t *mascot)
{
    if (!mascot) return;
    lv_anim_delete(mascot, NULL);                       /* 跳跃 */
    uint32_t n = lv_obj_get_child_count(mascot);
    for (uint32_t i = 0; i < n; i++) {                  /* 眼睛的眨眼循环 */
        lv_anim_delete(lv_obj_get_child(mascot, (int32_t)i), NULL);
    }
}

void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled)
{
    const ui_theme_t *th = ui_pixel_theme();
    uint32_t color = !enabled ? th->dim : (selected ? th->accent : th->panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(panel,
        lv_color_hex(selected ? 0xFFFFFF : th->ink), 0);
}
