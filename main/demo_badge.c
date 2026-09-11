// main/demo_badge.c —— 工牌主页,三套布局,上/下短按循环切换(只经过设置里开启的布局):
//   A 名片  头像(门户上传,缺省为吉祥物)+姓名/公司/岗位(默认隐藏)+时间
//   B 二维码 2x2 四槽,每槽可自由选"网页生成"(门户填文本)或"上传图片",
//           槽下有文字标识(门户填,缺省 QR1..QR4)
//   C 宠物  黑白像素宠物机(拓麻歌子式):蛋孵化→幼年→少年→成年,
//           饱食/心情/清洁/精力随时间衰减,便便要清理,睡觉回精力;
//           离线时长也会结算(NVS);动作模式「玩耍」进入小游戏:
//           猜方向(拓麻歌子传统)/快反应,赢局奖心情体重,开局耗精力
// 电量/音量/Wi-Fi 在每屏共用的顶部状态栏;自动息屏由 main.c 统一处理。
// 按键:▼ 长按 = 立即息屏;宠物页 OK 长按 = 进/出动作模式,动作模式下 ▲/▼ 选动作、
//       OK 短按执行;普通模式 ▲/▼ 切布局、OK 短按开菜单。
// 上传图片统一由门户 JS 居中裁成正方形:头像 96x96,二维码图 128x128 黑白(四槽)。
#include "demo.h"
#include "app_clock.h"
#include "app_config.h"
#include "app_pet.h"
#include "app_store.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
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
#define QR_SHOW       96       // 二维码槽显示边长(2x2 四槽)
#define QR_A8_BYTES   (QR_SHOW * QR_SHOW)   // A8 透明度画布,上传图每槽一份

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
#define QR_PAGE_BYTES    (QRIMG_FILE_BYTES + 4 * QR_A8_BYTES)
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

// 门户 JS 打包的 1bpp 为 LSB 在前(像素 i 落在第 i/8 字节的第 i&7 位)。
// 展开为 A8 透明度画布(255 = 模块),配合 image_recolor 染成黑色,比 RGB565 省 4 倍内存。
static void qrimg_expand_a8(const uint8_t *file, uint8_t *dst) {
    for (int y = 0; y < QR_SHOW; y++) {
        const uint8_t *row = file + (size_t)(y * QRIMG_SRC / QR_SHOW) * (QRIMG_SRC / 8);
        for (int x = 0; x < QR_SHOW; x++) {
            int sx = x * QRIMG_SRC / QR_SHOW;
            dst[y * QR_SHOW + x] = ((row[sx >> 3] >> (sx & 7)) & 1) ? 0xFF : 0x00;
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

static void build_qr(const app_config_t *cfg, const ui_theme_t *th) {
    // 2x2 四槽:每槽可自由选"网页生成"(门户填文本)或"上传图片"(如微信名片)
    uint8_t *file = s_page_buf;
    uint8_t *a8 = s_page_buf + QRIMG_FILE_BYTES;

    for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
        int x = 10 + (i % 2) * 124;
        int y = 54 + (i / 2) * 116;
        rect(s_scr, x, y, QR_SHOW, QR_SHOW, 0xFFFFFF);   // 扫码需要白底,主题无关

        bool img_mode = (cfg->qr_mode >> i) & 1;
        bool filled = false;
        if (img_mode && s_page_buf) {
            char name[12];
            snprintf(name, sizeof(name), "qr%d.img", i);
            if (app_store_read(name, file, QRIMG_FILE_BYTES) == QRIMG_FILE_BYTES) {
                qrimg_expand_a8(file, a8 + (size_t)i * QR_A8_BYTES);
                lv_obj_t *cv = lv_canvas_create(s_scr);
                lv_canvas_set_buffer(cv, a8 + (size_t)i * QR_A8_BYTES,
                                     QR_SHOW, QR_SHOW, LV_COLOR_FORMAT_A8);
                lv_obj_set_pos(cv, x, y);
                // A8 以 image_recolor 染色:模块染黑,透明处露出白底
                lv_obj_set_style_image_recolor(cv, lv_color_hex(0x000000), 0);
                lv_obj_set_style_image_recolor_opa(cv, LV_OPA_COVER, 0);
                filled = true;
            }
        } else if (!img_mode && cfg->qr_text[i][0] != '\0') {
            s_qr = lv_qrcode_create(s_scr);
            lv_qrcode_set_size(s_qr, QR_SHOW);
            lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
            lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
            lv_obj_set_pos(s_qr, x, y);
            lv_qrcode_update(s_qr, cfg->qr_text[i], strlen(cfg->qr_text[i]));
            filled = true;
        }
        if (!filled) {
            lv_obj_t *ph = ui_pixel_label(s_scr, ui_text(UI_T_QR_UNSET), &font_cjk_16, th->muted);
            lv_obj_set_pos(ph, x + 16, y + 40);
        }

        // 槽标签:门户里填的说明(如"微信"),缺省为 QR1..QR4(语言无关)
        char lbl[APP_CFG_QRLBL_LEN + 8];
        if (cfg->qr_label[i][0] != '\0') {
            snprintf(lbl, sizeof(lbl), "%s", cfg->qr_label[i]);
        } else {
            snprintf(lbl, sizeof(lbl), "QR%d", i + 1);
        }
        lv_obj_t *l = ui_pixel_label(s_scr, lbl, &font_cjk_16, th->muted);
        lv_obj_set_pos(l, x, y + QR_SHOW + 4);
        lv_obj_set_width(l, QR_SHOW);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
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

// ---- 小游戏用图(猜方向:两扇门+选中条+探头;快反应:苹果) ----

static const pet_sprite_t SPR_DOOR = { .rows = {      // 关闭的门(左开缝当门轴)
    "XXXXXXXXXXXXXXXX",
    "X..............X",
    "X..............X",
    "X..............X",
    "X..............X",
    "X..............X",
    "X.............XX",
    "X..............X",
    "X.............XX",
    "X..............X",
    "X..............X",
    "X..............X",
    "X..............X",
    "X..............X",
    "X..............X",
    "XXXXXXXXXXXXXXXX",
}};

static const pet_sprite_t SPR_MARK = { .rows = {      // 选中标记(门上方横条)
    "XXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXX",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
}};

static const pet_sprite_t SPR_APPLE = { .rows = {     // 快反应目标:苹果
    "................",
    "................",
    "................",
    "........X.......",
    ".......XX.......",
    "....XX..XX......",
    "...XXXXXXXXX....",
    "..XXXXXXXXXXX...",
    "..XXXXXXXXXXX...",
    "..XXXXXXXXXXX...",
    "..XXXXXXXXXXX...",
    "..XXXXXXXXXXX...",
    "...XXXXXXXXX....",
    "....XXXXXXX.....",
    "................",
    "................",
}};

static lv_obj_t *s_pet_canvas, *s_pet_mood, *s_pet_info, *s_pet_action;
static lv_obj_t *s_pet_bars[4];
static int s_pet_sel;                     // 当前选中动作 0..3
static bool s_pet_adjust;                 // 动作模式(OK 长按进出);退出时 ▲/▼ 恢复切换布局
static uint8_t s_pet_frame;               // 动画帧
static uint8_t s_pet_beats;               // 结算节拍计数

static void game_refresh_text(void);      // 前向声明:日常文字刷新在游戏期间要转发给游戏

// ---- 小游戏(动作模式里选「玩耍」进入) ----
// 猜方向:两扇门,▲/▼ 选左/右,OK 确认,宠物从一侧探头,猜中赢;快反应:苹果
// 随机时刻出现,900ms 内按 OK 算赢。均 5 局,结算走 pet_model_game_finish。
enum { GAME_NONE, GAME_SELECT, GAME_GUESS, GAME_TAP };
#define GAME_ROUNDS    5
#define GAME_TAP_WIN_MS 900
static int s_game;
static int s_gsel;                        // GAME_SELECT:0 猜一猜 1 快反应
static int s_round, s_wins;               // 当前局(0 基)/已胜局
static int s_phase;                       // 各游戏内部阶段
static int s_pick, s_reveal;              // 猜方向:玩家选择 / 宠物真实方向
static int s_beats;                       // 猜方向:揭晓停留节拍(700ms/拍)
static int s_tap_ticks;                   // 快反应:50ms 精确计数
static int s_tap_wait;                    // 苹果出现前的等待 tick 数
static lv_timer_t *s_tap_timer;           // 快反应 50ms 定时器(开局建,收尾删)
static bool s_last_win;                   // 上一局结果(文字显示用)
static int s_last_ms;                     // 快反应上一局反应时间

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
    if (s_game != GAME_NONE) {                 // 游戏期间三行文字归游戏
        game_refresh_text();
        return;
    }
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

    // 动作行:动作模式 = 高亮 + ▶ 前缀;普通模式 = 置灰(提示长按 OK 进入)
    static const ui_str_id ACTS[4] = { UI_T_ACT_FEED, UI_T_ACT_PLAY, UI_T_ACT_CLEAN, UI_T_ACT_SLEEP };
    const char *act = ui_text_lang(lang, ACTS[s_pet_sel]);
    if (s_pet_sel == 3 && (p->flags & PET_FLAG_ASLERP)) act = ui_text_lang(lang, UI_T_ACT_WAKE);
    char actline[64];
    if (lang == 0) snprintf(actline, sizeof(actline), "动作:%s", act);
    else           snprintf(actline, sizeof(actline), "Action: %s", act);
    const ui_theme_t *thm = ui_pixel_theme();
    lv_obj_set_style_text_color(s_pet_action,
                                lv_color_hex(s_pet_adjust ? thm->accent : thm->muted), 0);
    lv_label_set_text_fmt(s_pet_action, "%s%s", s_pet_adjust ? "▶ " : "", actline);
}

static void pet_refresh_bars(void) {
    const pet_state_t *p = app_pet_get();
    uint8_t vals[4] = { p->hunger, p->happy, p->clean, p->energy };
    for (int i = 0; i < 4; i++) {
        int w = ((int)vals[i] * 86) / 100;
        lv_obj_set_width(s_pet_bars[i], vals[i] ? (w < 3 ? 3 : w) : 0);
    }
}

/* ==================== 小游戏(拓麻歌子式:玩耍 → 小游戏,赢局奖心情) ==================== */

// 游戏期间画布由本函数族接管;pet_tick 不再画日常帧

static void game_render(void) {
    const ui_theme_t *th = ui_pixel_theme();
    uint16_t fg = rgb888_to_565(th->ink);
    uint16_t bg = rgb888_to_565(th->panel);
    for (int i = 0; i < PET_W * PET_H; i++) ((uint16_t *)s_page_buf)[i] = bg;

    if (s_game == GAME_SELECT) {
        pet_sprite_render(s_pet_frame ? &SPR_CHILD_B : &SPR_CHILD_A, 3, 4, 8, fg, bg);
    } else if (s_game == GAME_GUESS) {
        if (s_phase <= 1) {
            // 两扇门:被猜中侧在揭晓时换成探头本体
            for (int side = 0; side < 2; side++) {
                bool open = (s_phase == 1 && side == s_reveal);
                pet_sprite_render(open ? &SPR_CHILD_A : &SPR_DOOR, 2, 12, side * 32, fg, bg);
            }
            if (s_phase == 0) pet_sprite_render(&SPR_MARK, 2, 2, s_pick * 32, fg, bg);
        }
    } else if (s_game == GAME_TAP) {
        if (s_phase == 0) pet_sprite_render(&SPR_CHILD_A, 2, 4, 16, fg, bg);
        else if (s_phase == 1) pet_sprite_render(&SPR_APPLE, 2, 8, s_reveal, fg, bg);
    }
    if (s_pet_canvas) lv_obj_invalidate(s_pet_canvas);
}

// 游戏期间三行文字(覆盖日常的心情/信息/动作行)
static void game_refresh_text(void) {
    unsigned lang = app_config_get()->lang;
    char buf[48];
    if (s_game == GAME_SELECT) {
        lv_label_set_text(s_pet_mood, ui_text_lang(lang, UI_T_G_WHAT));
        lv_label_set_text(s_pet_info, "");
        if (lang == 0) snprintf(buf, sizeof(buf), "%s 猜一猜   %s 快反应",
                                s_gsel == 0 ? "▶" : " ", s_gsel == 1 ? "▶" : " ");
        else snprintf(buf, sizeof(buf), "%s Guess   %s React",
                      s_gsel == 0 ? ">" : " ", s_gsel == 1 ? ">" : " ");
        lv_label_set_text(s_pet_action, buf);
        return;
    }
    if (s_game == GAME_GUESS) {
        lv_label_set_text(s_pet_mood, ui_text_lang(lang, UI_T_G_GUESS));
        if (lang == 0) snprintf(buf, sizeof(buf), "第%d/%d局 胜%d",
                                s_round + 1, GAME_ROUNDS, s_wins);
        else snprintf(buf, sizeof(buf), "Round %d/%d won %d",
                      s_round + 1, GAME_ROUNDS, s_wins);
        lv_label_set_text(s_pet_info, buf);
        if (s_phase == 0) lv_label_set_text(s_pet_action, ui_text_lang(lang, UI_T_G_PICK));
        else if (s_phase == 1)
            lv_label_set_text(s_pet_action, ui_text_lang(lang, s_last_win ? UI_T_G_WIN : UI_T_G_LOSE));
        else lv_label_set_text(s_pet_action, ui_text_lang(lang, UI_T_G_OVER));
        return;
    }
    if (s_game == GAME_TAP) {
        lv_label_set_text(s_pet_mood, ui_text_lang(lang, UI_T_G_TAP));
        if (lang == 0) snprintf(buf, sizeof(buf), "第%d/%d局 胜%d",
                                s_round + 1, GAME_ROUNDS, s_wins);
        else snprintf(buf, sizeof(buf), "Round %d/%d won %d",
                      s_round + 1, GAME_ROUNDS, s_wins);
        lv_label_set_text(s_pet_info, buf);
        if (s_phase == 0) lv_label_set_text(s_pet_action, ui_text_lang(lang, UI_T_G_TAP_WAIT));
        else if (s_phase == 1) lv_label_set_text(s_pet_action, ui_text_lang(lang, UI_T_G_TAP_NOW));
        else if (s_phase == 2) {
            if (s_last_win) {
                if (lang == 0) snprintf(buf, sizeof(buf), "%d毫秒 赢!", s_last_ms);
                else snprintf(buf, sizeof(buf), "%d ms win!", s_last_ms);
            } else {
                snprintf(buf, sizeof(buf), "%s", ui_text_lang(lang, UI_T_G_TAP_MISS));
            }
            lv_label_set_text(s_pet_action, buf);
        } else lv_label_set_text(s_pet_action, ui_text_lang(lang, UI_T_G_OVER));
    }
}

// 收尾:删定时器、回日常画面;s_game 归零
static void game_stop(void) {
    if (s_tap_timer) { lv_timer_delete(s_tap_timer); s_tap_timer = NULL; }
    s_game = GAME_NONE;
    s_phase = 0;
    pet_render_sprite();
    pet_refresh_text();
}

static void game_end_apply(void) {
    pet_model_game_finish(app_pet_mut(), s_wins, GAME_ROUNDS);
    app_pet_save();
    pet_refresh_bars();
}

// 猜方向:揭晓停 2 拍、终局停 4 拍,由 pet_tick 的 700ms 节拍驱动
static void game_guess_tick(void) {
    if (s_phase == 1) {
        if (++s_beats >= 2) {
            s_beats = 0;
            s_round++;
            if (s_round >= GAME_ROUNDS) {
                s_phase = 2;
                game_end_apply();
            } else {
                s_phase = 0;
            }
            game_render();
            game_refresh_text();
        }
    } else if (s_phase == 2 && ++s_beats >= 4) {
        game_stop();
    }
}

static void game_guess_pick(int side) {
    if (s_phase != 0 || s_pick == side) return;
    s_pick = side;
    game_render();
}

static void game_guess_ok(void) {
    if (s_phase != 0) return;
    s_reveal = (int)(esp_random() & 1);
    s_last_win = (s_reveal == s_pick);
    if (s_last_win) s_wins++;
    s_phase = 1;
    s_beats = 0;
    game_render();
    game_refresh_text();
}

// 快反应一局结束:胜利记反应时间;推进局数/终局
static void game_tap_round_done(bool win, int ms) {
    s_last_win = win;
    s_last_ms = ms;
    if (win) s_wins++;
    s_tap_ticks = 0;
    s_round++;
    if (s_round >= GAME_ROUNDS) {
        s_phase = 3;
        game_end_apply();
    } else {
        s_phase = 2;                       // 0.5 秒反馈停留后进下一局
    }
    game_render();
    game_refresh_text();
}

// 快反应 50ms 节拍:等待→苹果出现(最多 2 秒)→反馈→下一局/终局
static void game_tap_tick(lv_timer_t *t) {
    (void)t;
    s_tap_ticks++;
    if (s_phase == 0 && s_tap_ticks >= s_tap_wait) {
        s_phase = 1;
        s_tap_ticks = 0;
        s_reveal = (int)(esp_random() % 33);
        game_render();
        game_refresh_text();
    } else if (s_phase == 1 && s_tap_ticks > 40) {   // 2 秒没按
        game_tap_round_done(false, 0);
    } else if (s_phase == 2 && s_tap_ticks >= 10) {
        s_phase = 0;
        s_tap_ticks = 0;
        s_tap_wait = 16 + (int)(esp_random() % 40);
        game_render();
        game_refresh_text();
    } else if (s_phase == 3 && s_tap_ticks >= 10) {
        game_stop();
    }
}

static void game_tap_ok(void) {
    if (s_phase == 0) {                    // 苹果还没出就按:抢跑
        game_tap_round_done(false, 0);
    } else if (s_phase == 1) {
        int ms = s_tap_ticks * 50;
        game_tap_round_done(ms <= GAME_TAP_WIN_MS, ms);
    }
}

// 动作模式选「玩耍」确认后进入:先出小游戏选择菜单
static void game_open_select(void) {
    s_game = GAME_SELECT;
    s_gsel = 0;
    game_render();
    game_refresh_text();
}

static void game_begin(int sel) {
    pet_state_t *p = app_pet_mut();
    if (!app_clock_synced() || !pet_model_game_start(p)) {
        lv_label_set_text(s_pet_action, ui_text(UI_T_G_NOENERGY));
        return;
    }
    app_pet_save();
    pet_refresh_bars();
    s_round = 0;
    s_wins = 0;
    s_last_win = false;
    s_last_ms = 0;
    s_game = (sel == 0) ? GAME_GUESS : GAME_TAP;
    if (s_game == GAME_GUESS) {
        s_phase = 0;
        s_pick = 0;
    } else {
        s_phase = 0;
        s_tap_ticks = 0;
        s_tap_wait = 16 + (int)(esp_random() % 40);
        s_tap_timer = lv_timer_create(game_tap_tick, 50, NULL);
    }
    game_render();
    game_refresh_text();
}

// OK 长按退出动作模式时,若在游戏中则中止(开局扣的精力不退)
static void game_cancel(void) {
    if (s_game != GAME_NONE) game_stop();
}

// 宠物页每拍(700ms):帧动画 + 周期性数值结算;猜方向游戏的节拍也挂在这里
static void pet_tick(lv_timer_t *t) {
    (void)t;
    if (s_game == GAME_GUESS) game_guess_tick();
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
    if (s_game == GAME_NONE) pet_render_sprite();
    else if (s_game == GAME_SELECT) game_render();
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

// 宠物页"动作模式"按键:▲/▼ 选动作,OK 执行(仅在 s_pet_adjust 时被调用);
// 游戏进行中按键先归游戏
static void pet_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (s_game == GAME_SELECT) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_gsel = 1 - s_gsel;
            game_refresh_text();
        } else if (btn == BSP_BTN_OK) {
            game_begin(s_gsel);
        }
        return;
    }
    if (s_game == GAME_GUESS) {
        if (btn == BSP_BTN_UP) game_guess_pick(0);
        else if (btn == BSP_BTN_DOWN) game_guess_pick(1);
        else if (btn == BSP_BTN_OK) game_guess_ok();
        return;
    }
    if (s_game == GAME_TAP) {
        if (btn == BSP_BTN_OK) game_tap_ok();
        return;
    }
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
    case 1: game_open_select(); return;      // 玩耍 → 小游戏菜单
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
        s_page_buf = heap_caps_malloc(QR_PAGE_BYTES, MALLOC_CAP_8BIT);
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
            { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_VIEW), false },  // 布局/选动作
            { "OK",                        ui_text(UI_T_MENU), false },  // 菜单/执行
            { "OK",                        ui_text(UI_T_ADJ),  true  },  // 长按进/出动作模式
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
    if (s_tap_timer) { lv_timer_delete(s_tap_timer); s_tap_timer = NULL; }
    s_game = GAME_NONE;
    s_pet_adjust = false;              // 重进页面从普通模式开始
}

void demo_badge_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        demo_request_screen_off();
        return;
    }
    if (app_config_get()->layout == APP_LAYOUT_PET) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            s_pet_adjust = !s_pet_adjust;   // OK 长按 = 进/出动作模式
            if (!s_pet_adjust) game_cancel();  // 退出动作模式时中止进行中的游戏
            pet_refresh_text();
            return;
        }
        if (s_pet_adjust) {                 // 动作模式消化全部按键
            pet_key(btn, ev);
            return;
        }
        // 普通模式:OK 短按与 ▲/▼ 走下方统一逻辑(菜单 / 切布局)
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        demo_request_menu();                 // 本页对象已被 exit 删除,之后不能再碰
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
