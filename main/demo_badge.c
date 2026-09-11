// main/demo_badge.c —— 工牌主页,三套布局,上/下短按循环切换(只经过设置里开启的布局):
//   A 名片  头像(门户上传,缺省为吉祥物)+姓名/公司/岗位(默认隐藏)+时间
//   B 二维码 左=链接生成码(门户"二维码内容"),右=上传的二维码图(如微信),带文字标识
//   C 宠物  黑白像素宠物机(拓麻歌子式):蛋孵化→幼年→少年→成年,
//           饱食/心情/清洁/精力随时间衰减,便便要清理,睡觉回精力;
//           ▲/▼ 选动作(喂食/玩耍/清洁/睡觉),OK 执行;离线时长也会结算(NVS)
// 电量/音量/Wi-Fi 在每屏共用的顶部状态栏;自动息屏由 main.c 统一处理。
// 按键:OK 短按 = 打开菜单(宠物页内 = 执行动作);▼ 长按 = 立即息屏。
// 上传图片统一由门户 JS 居中裁成正方形:头像 96x96,二维码图 128x128 黑白。
#include "demo.h"
#include "app_clock.h"
#include "app_config.h"
#include "app_pet.h"
#include "app_store.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLOCK_TICK_MS 1000
#define PET_TICK_MS   700      // 宠物帧动画节拍;每 30 拍结算一次数值
#define AVATAR_SRC    96       // 门户上传原图边长(RGB565 LE)
#define AVATAR_SHOW   64       // 名片头像显示边长(最近邻缩小)
#define QRIMG_SRC     128      // 门户上传 1bpp 位图边长
#define QRIMG_SHOW    104      // 与左侧生成码同宽

static lv_obj_t *s_scr;
static lv_obj_t *s_name, *s_org, *s_title;
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_mascot, *s_qr;
static lv_timer_t *s_timer;
static bool s_has_mascot;

// 大缓冲进页按需分配、退页释放,平时不占堆:曾因常驻 ~60KB 静态把 Wi-Fi 驱动的
// RX 缓冲挤出内存(开机 esp_wifi_init 报 NO_MEM,设备永不联网)。
// 按布局精确分配,尽量少占:名片 = 缩放副本常驻,原图临时用完即释放;
// 二维码 = 1bpp 文件+展开副本;宠物 = 64x64 点阵画布。
#define AVATAR_SRC_BYTES (AVATAR_SRC * AVATAR_SRC * 2)
#define AVATAR_DST_BYTES (AVATAR_SHOW * AVATAR_SHOW * 2)
#define QRIMG_FILE_BYTES (QRIMG_SRC * QRIMG_SRC / 8)
#define QRIMG_DST_BYTES  (QRIMG_SHOW * QRIMG_SHOW * 2)
#define PET_W            64
#define PET_H            64
#define PET_BYTES        (PET_W * PET_H * 2)
static uint8_t *s_page_buf;      // 当前布局的显示缓冲
static uint8_t *s_avatar_src;    // 名片布局的原图临时缓冲,缩放后立即释放

// 最近邻抽样缩小 RGB565 LE(LV_COLOR_DEPTH=16 且无 swap,缓冲可直接作为画布)
static void shrink565(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    uint16_t *out = (uint16_t *)dst;
    for (int y = 0; y < dh; y++) {
        const uint8_t *srow = src + (size_t)(y * sh / dh) * sw * 2;
        for (int x = 0; x < dw; x++) {
            const uint8_t *px = srow + (size_t)(x * sw / dw) * 2;
            *out++ = (uint16_t)px[0] | ((uint16_t)px[1] << 8);
        }
    }
}

// 门户 JS 打包的 1bpp 为 LSB 在前(像素 i 落在第 i/8 字节的第 i&7 位),展开为黑/白 RGB565
static void qrimg_expand(const uint8_t *file, uint8_t *dst) {
    uint16_t *out = (uint16_t *)dst;
    for (int y = 0; y < QRIMG_SHOW; y++) {
        const uint8_t *row = file + (size_t)(y * QRIMG_SRC / QRIMG_SHOW) * (QRIMG_SRC / 8);
        for (int x = 0; x < QRIMG_SHOW; x++) {
            int sx = x * QRIMG_SRC / QRIMG_SHOW;
            *out++ = ((row[sx >> 3] >> (sx & 7)) & 1) ? 0x0000 : 0xFFFF;
        }
    }
}

static void clock_tick(lv_timer_t *t) {
    (void)t;
    struct tm lt;
    unsigned lang = app_config_get()->lang;
    if (app_clock_local(&lt)) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d", lt.tm_hour, lt.tm_min);
        lv_label_set_text_fmt(s_date, "%02d-%02d %s", lt.tm_mon + 1, lt.tm_mday,
                              ui_text_weekday(lang, lt.tm_wday));
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, ui_text(UI_T_NO_TIME));
    }
}

static lv_obj_t *text_line(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, int x, int y, int w) {
    lv_obj_t *label = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

// 纯色矩形(状态条等像素件);清除默认样式后仅保留背景
static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

static void build_clock(const ui_theme_t *th, int y) {
    lv_obj_t *bar = ui_pixel_panel_create(s_scr, 11, y, 218, 52, th->panel);
    s_clock = ui_pixel_label(bar, "--:--", &lv_font_montserrat_28, th->ink);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);
    s_date = ui_pixel_label(bar, ui_text(UI_T_NO_TIME), &font_cjk_16, th->muted);
    lv_obj_align(s_date, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_card(const app_config_t *cfg, const ui_theme_t *th) {
    lv_obj_t *card = ui_pixel_panel_create(s_scr, 11, 52, 218, 118, th->panel);

    // 头像:上传图缩小为 64x64 贴画布;未上传或内存不足时保持吉祥物
    uint8_t *dst = s_page_buf;
    int n = (s_page_buf && s_avatar_src)
                ? app_store_read("avatar.rgb565", s_avatar_src, AVATAR_SRC_BYTES) : -1;
    if (n == AVATAR_SRC_BYTES) {
        shrink565(s_avatar_src, AVATAR_SRC, AVATAR_SRC, dst, AVATAR_SHOW, AVATAR_SHOW);
        free(s_avatar_src);              // 原图用完即释放
        s_avatar_src = NULL;
        lv_obj_t *cv = lv_canvas_create(card);
        lv_canvas_set_buffer(cv, dst, AVATAR_SHOW, AVATAR_SHOW,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(cv, 0, 0);
    } else {
        if (s_avatar_src) { free(s_avatar_src); s_avatar_src = NULL; }
        lv_obj_t *avatar = ui_pixel_panel_create(card, 0, 0, 64, 64, th->dim);
        s_mascot = ui_pixel_mascot_create(avatar, 6, 1);
        s_has_mascot = true;
    }
    s_name  = text_line(card, cfg->name,  &font_cjk_20, th->ink,   78, 2,  116);
    s_org   = text_line(card, cfg->org,   &font_cjk_16, th->muted, 78, 32, 116);
    s_title = text_line(card, cfg->title, &font_cjk_16, th->muted, 78, 54, 116);
    if (cfg->hide_org_title) {
        lv_obj_add_flag(s_org, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    }
    build_clock(th, 182);
}

// 二维码说明文字(居中于各自码的下方)
static lv_obj_t *qr_caption(const ui_theme_t *th, const char *text, int x) {
    lv_obj_t *l = ui_pixel_label(s_scr, text, &font_cjk_16, th->ink);
    lv_obj_set_pos(l, x, 170);
    lv_obj_set_width(l, 104);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void build_qr(const app_config_t *cfg, const ui_theme_t *th) {
    // 左:链接生成码(门户"二维码内容");右:上传的二维码图(如微信名片)
    bool has = cfg->qr_a[0] != '\0';
    s_qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qr, 104);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_qr, 14, 60);
    if (has) lv_qrcode_update(s_qr, cfg->qr_a, strlen(cfg->qr_a));
    qr_caption(th, ui_text(UI_T_QR_LINK), 14);

    uint8_t *file = s_page_buf, *dst = s_page_buf + QRIMG_FILE_BYTES;
    int n = s_page_buf ? app_store_read("qr_b.img", file, QRIMG_FILE_BYTES) : -1;
    if (n == QRIMG_FILE_BYTES) {
        qrimg_expand(file, dst);
        lv_obj_t *cv = lv_canvas_create(s_scr);
        lv_canvas_set_buffer(cv, dst, QRIMG_SHOW, QRIMG_SHOW,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(cv, 122, 60);
    } else {
        lv_obj_t *ph = ui_pixel_label(s_scr, ui_text(UI_T_QR_NONE), &font_cjk_16, th->muted);
        lv_obj_set_pos(ph, 154, 96);
    }
    qr_caption(th, ui_text(UI_T_QR_IMG), 122);
}

/* ==================== 像素宠物布局(拓麻歌子式) ==================== */

// 黑白点阵精灵:'X' = 前景像素,其余为背景;统一 16x16 网格
#define SPR_W 16
#define SPR_H 16
typedef struct {
    const char *rows[SPR_H];
} pet_sprite_t;

static const pet_sprite_t SPR_EGG = { .rows = {
    "................",
    "................",
    ".....XXXXXX.....",
    "....X......X....",
    "...X........X...",
    "...X...XX...X...",
    "..X..........X..",
    "..X..........X..",
    "..X.....XX...X..",
    "..X..........X..",
    "...X...XX...X...",
    "...X........X...",
    "....X......X....",
    ".....XXXXXX.....",
    "................",
    "................",
}};

static const pet_sprite_t SPR_BABY_A = { .rows = {
    "................",
    "................",
    "................",
    "................",
    "................",
    "......XXXX......",
    ".....XXXXXX.....",
    "....XXXXXXXX....",
    "....X.XX.XXX....",
    "....XXXXXXXX....",
    ".....XXXXXX.....",
    "......XXXX......",
    "................",
    "................",
    "................",
    "................",
}};

static const pet_sprite_t SPR_BABY_B = { .rows = {   // 跳动帧(整体上移一格)
    "................",
    "................",
    "................",
    "................",
    "......XXXX......",
    ".....XXXXXX.....",
    "....XXXXXXXX....",
    "....X.XX.XXX....",
    "....XXXXXXXX....",
    ".....XXXXXX.....",
    "......XXXX......",
    "................",
    "................",
    "................",
    "................",
    "................",
}};

static const pet_sprite_t SPR_CHILD_A = { .rows = {
    "................",
    "................",
    ".......XX.......",
    ".......XX.......",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "...XX.XXXX.XX...",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    "....XX....XX....",
    "....XX....XX....",
    "................",
    "................",
    "................",
    "................",
}};

static const pet_sprite_t SPR_CHILD_B = { .rows = {   // 眨眼帧
    "................",
    "................",
    ".......XX.......",
    ".......XX.......",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    "....XX....XX....",
    "....XX....XX....",
    "................",
    "................",
    "................",
    "................",
}};

static const pet_sprite_t SPR_ADULT_A = { .rows = {
    "................",
    ".......XX.......",
    ".......XX.......",
    "...XXXXXXXXXX...",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "..XXX......XXX..",
    "..XXX.X..X.XXX..",
    "..XXX.XXXX.XXX..",
    "..XXX......XXX..",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "....XX....XX....",
    "....XX....XX....",
    "................",
}};

static const pet_sprite_t SPR_ADULT_B = { .rows = {   // 眨眼帧
    "................",
    ".......XX.......",
    ".......XX.......",
    "...XXXXXXXXXX...",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "..XXX......XXX..",
    "..XXX......XXX..",
    "..XXX.XXXX.XXX..",
    "..XXX......XXX..",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "....XX....XX....",
    "....XX....XX....",
    "................",
}};

static const pet_sprite_t SPR_POOP = { .rows = {      // 8 宽小图,叠加在角落
    "........",
    "...XX...",
    "..XXXX..",
    "..XXXX..",
    ".XXXXXX.",
    "XXXXXXXX",
    "........",
    "........",
}};

static lv_obj_t *s_pet_canvas, *s_pet_mood, *s_pet_info, *s_pet_action;
static lv_obj_t *s_pet_bars[4];
static int s_pet_sel;                     // 当前选中动作 0..3
static uint8_t s_pet_frame;               // 动画帧
static uint8_t s_pet_beats;               // 结算节拍计数

static uint16_t rgb888_to_565(uint32_t c) {
    return (uint16_t)(((c & 0xF80000) >> 8) | ((c & 0x00FC00) >> 5) |
                      ((c & 0x0000F8) >> 3));
}

// 把点阵精灵按 scale 放大画进 64x64 RGB565 画布;top/left 为画布内偏移
static void pet_sprite_render(const pet_sprite_t *sp, int scale, int top, int left,
                              uint16_t fg, uint16_t bg) {
    uint16_t *out = (uint16_t *)s_page_buf;
    for (int y = 0; y < SPR_H * scale; y++) {
        int sy = top + y;
        if (sy < 0 || sy >= PET_H) continue;
        const char *row = sp->rows[y / scale];
        size_t len = row ? strlen(row) : 0;
        for (int x = 0; x < SPR_W * scale; x++) {
            int sx = left + x;
            if (sx < 0 || sx >= PET_W) continue;
            out[sy * PET_W + sx] =
                ((size_t)(x / scale) < len && row[x / scale] == 'X') ? fg : bg;
        }
    }
}

static void pet_render_sprite(void) {
    const ui_theme_t *th = ui_pixel_theme();
    uint16_t fg = rgb888_to_565(th->ink);
    uint16_t bg = rgb888_to_565(th->panel);
    uint32_t now = app_clock_synced() ? (uint32_t)time(NULL) : 0;

    for (int i = 0; i < PET_W * PET_H; i++) ((uint16_t *)s_page_buf)[i] = bg;

    const pet_state_t *p = app_pet_get();
    const pet_sprite_t *sp;
    switch (pet_model_stage(p, now)) {
    case PET_STAGE_BABY:  sp = s_pet_frame ? &SPR_BABY_B : &SPR_BABY_A; break;
    case PET_STAGE_CHILD: sp = s_pet_frame ? &SPR_CHILD_B : &SPR_CHILD_A; break;
    case PET_STAGE_ADULT: sp = s_pet_frame ? &SPR_ADULT_B : &SPR_ADULT_A; break;
    default:              sp = &SPR_EGG; break;
    }
    pet_sprite_render(sp, 4, 0, 0, fg, bg);

    if (p->flags & PET_FLAG_POOP) pet_sprite_render(&SPR_POOP, 2, 46, 46, fg, bg);

    if (s_pet_canvas) lv_obj_invalidate(s_pet_canvas);
}

static const char *pet_stage_txt(pet_stage_t stage, unsigned lang) {
    switch (stage) {
    case PET_STAGE_BABY:  return ui_text_lang(lang, UI_T_PET_STAGE_BABY);
    case PET_STAGE_CHILD: return ui_text_lang(lang, UI_T_PET_STAGE_CHILD);
    case PET_STAGE_ADULT: return ui_text_lang(lang, UI_T_PET_STAGE_ADULT);
    default:              return ui_text_lang(lang, UI_T_PET_STAGE_EGG);
    }
}

static void pet_refresh_text(void) {
    const app_config_t *cfg = app_config_get();
    unsigned lang = cfg->lang;
    const pet_state_t *p = app_pet_get();
    uint32_t now = app_clock_synced() ? (uint32_t)time(NULL) : 0;

    // 心情行
    const char *mood;
    if (!app_clock_synced()) mood = ui_text(UI_T_PET_MOOD_NA);
    else if (pet_model_stage(p, now) == PET_STAGE_EGG) mood = ui_text(UI_T_PET_HATCH);
    else {
        switch (pet_model_mood(p)) {
        case PET_MOOD_ASLEEP: mood = ui_text(UI_T_PET_MOOD_ASLEEP); break;
        case PET_MOOD_HUNGRY: mood = ui_text(UI_T_PET_MOOD_HUNGRY); break;
        case PET_MOOD_SLEEPY: mood = ui_text(UI_T_PET_MOOD_SLEEPY); break;
        case PET_MOOD_SAD:    mood = ui_text(UI_T_PET_MOOD_SAD);    break;
        case PET_MOOD_HAPPY:  mood = ui_text(UI_T_PET_MOOD_HAPPY);  break;
        case PET_MOOD_FINE:   mood = ui_text(UI_T_PET_MOOD_FINE);   break;
        default:              mood = ui_text(UI_T_PET_MOOD_NA);     break;
        }
    }
    lv_label_set_text(s_pet_mood, mood);

    // 信息行:阶段 · 龄 · 体重
    pet_stage_t stage = pet_model_stage(p, now);
    char buf[48];
    if (stage == PET_STAGE_EGG) {
        snprintf(buf, sizeof(buf), "%s", ui_text_lang(lang, UI_T_PET_STAGE_EGG));
    } else {
        char age[24];
        snprintf(age, sizeof(age), ui_text_lang(lang, UI_T_PET_AGE_FMT),
                 p->age_min / 1440, (p->age_min % 1440) / 60);
        snprintf(buf, sizeof(buf), "%s · %s · %dg", pet_stage_txt(stage, lang),
                 age, p->weight_g);
    }
    lv_label_set_text(s_pet_info, buf);

    // 动作行
    static const ui_str_id ACTS[4] = { UI_T_ACT_FEED, UI_T_ACT_PLAY, UI_T_ACT_CLEAN, UI_T_ACT_SLEEP };
    const char *act = ui_text_lang(lang, ACTS[s_pet_sel]);
    if (s_pet_sel == 3 && (p->flags & PET_FLAG_ASLERP)) act = ui_text_lang(lang, UI_T_ACT_WAKE);
    lv_label_set_text_fmt(s_pet_action, ui_text(UI_T_ACT_FMT), act);
}

static void pet_refresh_bars(void) {
    const pet_state_t *p = app_pet_get();
    uint8_t vals[4] = { p->hunger, p->happy, p->clean, p->energy };
    for (int i = 0; i < 4; i++) {
        int w = ((int)vals[i] * 86) / 100;
        lv_obj_set_width(s_pet_bars[i], vals[i] ? (w < 3 ? 3 : w) : 0);
    }
}

// 宠物页每拍(700ms):帧动画 + 周期性数值结算
static void pet_tick(lv_timer_t *t) {
    (void)t;
    if (++s_pet_beats >= 30) {                   // ~21 秒结算一次并落盘
        s_pet_beats = 0;
        if (app_clock_synced()) {
            pet_model_tick(app_pet_mut(), (uint32_t)time(NULL));
            app_pet_save();
            pet_refresh_bars();
        }
        pet_refresh_text();
    }
    s_pet_frame ^= 1;
    pet_render_sprite();
}

static void build_pet(const app_config_t *cfg, const ui_theme_t *th) {
    // 首次进入且已校时:生一颗蛋
    if (!app_pet_initialized() && app_clock_synced()) {
        app_pet_birth((uint32_t)time(NULL));
    }
    // 进页先结算离线时长
    if (app_pet_initialized() && app_clock_synced()) {
        pet_model_tick(app_pet_mut(), (uint32_t)time(NULL));
        app_pet_save();
    }

    lv_obj_t *card = ui_pixel_panel_create(s_scr, 11, 52, 218, 200, th->panel);
    lv_obj_t *cv = lv_canvas_create(card);
    lv_canvas_set_buffer(cv, s_page_buf, PET_W, PET_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, 2, 2);
    s_pet_canvas = cv;

    // 右侧四条状态:标签 + 底条 + 值条
    const ui_str_id labels[4] = { UI_T_ST_HUNGER, UI_T_ST_FUN, UI_T_ST_CLEAN, UI_T_ST_ENERGY };
    for (int i = 0; i < 4; i++) {
        int y = 6 + i * 20;
        lv_obj_t *l = ui_pixel_label(card, ui_text(labels[i]), &font_cjk_16, th->muted);
        lv_obj_set_pos(l, 70, y);
        rect(card, 108, y + 3, 86, 10, th->dim);
        s_pet_bars[i] = rect(card, 108, y + 3, 0, 10, th->accent);
    }
    pet_refresh_bars();

    s_pet_mood = ui_pixel_label(card, "", &font_cjk_16, th->ink);
    lv_obj_set_pos(s_pet_mood, 0, 92);
    lv_obj_set_width(s_pet_mood, 196);
    lv_obj_set_style_text_align(s_pet_mood, LV_TEXT_ALIGN_CENTER, 0);

    s_pet_info = ui_pixel_label(card, "", &font_cjk_16, th->muted);
    lv_obj_set_pos(s_pet_info, 0, 116);
    lv_obj_set_width(s_pet_info, 196);
    lv_obj_set_style_text_align(s_pet_info, LV_TEXT_ALIGN_CENTER, 0);

    s_pet_action = ui_pixel_label(card, "", &font_cjk_16, th->accent);
    lv_obj_set_pos(s_pet_action, 0, 140);
    lv_obj_set_width(s_pet_action, 196);
    lv_obj_set_style_text_align(s_pet_action, LV_TEXT_ALIGN_CENTER, 0);

    pet_render_sprite();
    pet_refresh_text();
}

// 宠物页按键:▲/▼ 换动作,OK 执行(OK 长按返回菜单由 main.c 全局拦截)
static void pet_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        s_pet_sel = (s_pet_sel + 4 + (btn == BSP_BTN_DOWN ? 1 : -1)) % 4;
        pet_refresh_text();
        return;
    }
    if (btn != BSP_BTN_OK || !app_clock_synced()) return;
    pet_state_t *p = app_pet_mut();
    uint32_t now = (uint32_t)time(NULL);
    bool did = false;
    switch (s_pet_sel) {
    case 0: did = pet_model_feed(p, now);  break;
    case 1: did = pet_model_play(p, now);  break;
    case 2: did = pet_model_clean(p, now); break;
    default: did = pet_model_toggle_sleep(p, now); break;
    }
    if (did) {
        app_pet_save();
        pet_refresh_bars();
        pet_refresh_text();
        pet_render_sprite();
    }
}

void demo_badge_enter(void) {
    const app_config_t *cfg = app_config_get();
    const ui_theme_t *th = ui_pixel_theme();
    s_has_mascot = false;
    s_page_buf = NULL;
    s_avatar_src = NULL;
    // 按布局的精确需求分配(失败则该布局降级为占位内容)
    switch (cfg->layout) {
    case APP_LAYOUT_QR:
        s_page_buf = heap_caps_malloc(QRIMG_FILE_BYTES + QRIMG_DST_BYTES, MALLOC_CAP_8BIT);
        break;
    case APP_LAYOUT_PET:
        s_page_buf = heap_caps_malloc(PET_BYTES, MALLOC_CAP_8BIT);
        break;
    case APP_LAYOUT_CARD:
        s_page_buf = heap_caps_malloc(AVATAR_DST_BYTES, MALLOC_CAP_8BIT);
        s_avatar_src = heap_caps_malloc(AVATAR_SRC_BYTES, MALLOC_CAP_8BIT);
        break;
    default:
        break;
    }
    s_scr = ui_pixel_screen_create("BADGE");

    switch (cfg->layout) {
    case APP_LAYOUT_QR:  build_qr(cfg, th);  break;
    case APP_LAYOUT_PET: build_pet(cfg, th); break;
    default:             build_card(cfg, th); break;
    }

    if (cfg->layout == APP_LAYOUT_PET) {
        const ui_hint_t PET_HINTS[] = {
            { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_SEL),  false },
            { "OK",                        ui_text(UI_T_ADJ),  false },
            { "OK",                        ui_text(UI_T_MENU), true  },
        };
        ui_pixel_hints(s_scr, PET_HINTS, 3);
        s_pet_frame = 0;
        s_pet_beats = 0;
        s_timer = lv_timer_create(pet_tick, PET_TICK_MS, NULL);
    } else {
        const ui_hint_t BADGE_HINTS[] = {
            { "OK",                        ui_text(UI_T_MENU),  false },
            { LV_SYMBOL_DOWN,              ui_text(UI_T_SLEEP), true  },
            { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_VIEW),  false },
        };
        ui_pixel_hints(s_scr, BADGE_HINTS, 3);
        if (cfg->layout == APP_LAYOUT_CARD) {
            // 只有名片布局显示时间
            clock_tick(NULL);
            s_timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, NULL);
        }
    }
    lv_screen_load(s_scr);
}

void demo_badge_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) {
        if (s_has_mascot) ui_pixel_mascot_stop(s_mascot);
        lv_obj_delete(s_scr);          // 先删屏(画布还指着页缓冲),再释放缓冲
        s_scr = NULL;
        s_name = s_org = s_title = s_clock = s_date = s_mascot = s_qr = NULL;
        s_pet_canvas = s_pet_mood = s_pet_info = s_pet_action = NULL;
    }
    if (s_page_buf) { free(s_page_buf); s_page_buf = NULL; }
    if (s_avatar_src) { free(s_avatar_src); s_avatar_src = NULL; }
}

void demo_badge_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        demo_request_menu();                 // 本页对象已被 exit 删除,之后不能再碰
        return;
    }
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        demo_request_screen_off();
        return;
    }
    if (app_config_get()->layout == APP_LAYOUT_PET) {
        pet_key(btn, ev);                    // 宠物页:▲/▼ 选动作,OK 执行
        return;
    }
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        app_config_t c = *app_config_get();
        int dir = (btn == BSP_BTN_DOWN) ? +1 : -1;
        c.layout = app_config_next_layout(c.layout, c.layout_mask, dir);
        app_config_update(&c);
        demo_badge_exit();
        demo_badge_enter();
    }
}
