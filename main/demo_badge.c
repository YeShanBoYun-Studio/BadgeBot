// main/demo_badge.c —— 工牌主页,四套布局,上/下短按循环切换(只经过设置里开启的布局):
//   A 名片   头像(门户上传,缺省为吉祥物)+姓名/公司/岗位(默认隐藏)+时间
//   B 二维码 左=链接生成码(门户"二维码内容"),右=上传的二维码图(如微信),带文字标识
//   C GitHub 提交热力图(app_github 缓存;联网窗口拉到数据后本页自动重绘)
//   D 宠物   大号吉祥物卡片(养成系统在 M7 逐步填充)
// 电量/音量/Wi-Fi 在每屏共用的顶部状态栏;自动息屏由 main.c 统一处理。
// 按键:OK 短按 = 打开菜单;▼ 长按 = 立即息屏;▲/▼ 短按 = 切换布局。
// 上传图片统一由门户 JS 居中裁成正方形:头像 96x96,二维码图 128x128 黑白。
#include "demo.h"
#include "app_clock.h"
#include "app_config.h"
#include "app_github.h"
#include "app_store.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_TICK_MS 1000
#define AVATAR_SRC    96        // 门户上传原图边长(RGB565 LE)
#define AVATAR_SHOW   64        // 名片头像显示边长(最近邻缩小)
#define QRIMG_SRC     128       // 门户上传 1bpp 位图边长
#define QRIMG_SHOW    104       // 与左侧生成码同宽
#define HEAT_W (53 * 4 - 1)     // 53 周 x 7 行,格 3px 间距 1px
#define HEAT_H (7 * 4 - 1)

static lv_obj_t *s_scr;
static lv_obj_t *s_name, *s_org, *s_title;
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_mascot, *s_qr;
static lv_timer_t *s_timer;
static bool s_has_mascot;
static bool s_gh_ready_at_enter;   // GitHub 布局进入时是否已有缓存(变化则自动重绘)

// 大缓冲进页按需分配、退页释放,平时不占堆:曾因常驻 ~60KB 静态把 Wi-Fi 驱动的
// RX 缓冲挤出内存(开机 esp_wifi_init 报 NO_MEM,设备永不联网)。
// 单块竞技场按布局切片:名片 = 头像原图+缩放副本;二维码 = 1bpp 文件+展开副本;热力图独用。
#define AVATAR_SRC_BYTES (AVATAR_SRC * AVATAR_SRC * 2)
#define AVATAR_DST_BYTES (AVATAR_SHOW * AVATAR_SHOW * 2)
#define QRIMG_FILE_BYTES (QRIMG_SRC * QRIMG_SRC / 8)
#define QRIMG_DST_BYTES  (QRIMG_SHOW * QRIMG_SHOW * 2)
#define HEAT_BYTES       (HEAT_W * HEAT_H * 2)
#define PAGE_BUF_MAX     (AVATAR_SRC_BYTES + AVATAR_DST_BYTES)   // 各布局中最大需求
static uint8_t *s_page_buf;

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

// GitHub 布局的 1 秒轮询:数据首次就绪(或从有到无)时整页重建,把热力图画上去
static void gh_tick(lv_timer_t *t) {
    (void)t;
    if (app_github_ready() == s_gh_ready_at_enter) return;
    demo_badge_exit();
    demo_badge_enter();
}

static lv_obj_t *text_line(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, int x, int y, int w) {
    lv_obj_t *label = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
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
    uint8_t *src = s_page_buf, *dst = s_page_buf + AVATAR_SRC_BYTES;
    int n = s_page_buf ? app_store_read("avatar.rgb565", src, AVATAR_SRC_BYTES) : -1;
    if (n == AVATAR_SRC_BYTES) {
        shrink565(src, AVATAR_SRC, AVATAR_SRC, dst, AVATAR_SHOW, AVATAR_SHOW);
        lv_obj_t *cv = lv_canvas_create(card);
        lv_canvas_set_buffer(cv, dst, AVATAR_SHOW, AVATAR_SHOW,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(cv, 0, 0);
    } else {
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

static void build_github(const app_config_t *cfg, const ui_theme_t *th) {
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 11, 52, 218, 130, th->panel);
    lv_obj_t *t1 = ui_pixel_label(panel, ui_text(UI_T_GH_TITLE), &font_cjk_20, th->ink);
    lv_obj_set_pos(t1, 4, 4);

    if (!app_github_ready()) {
        lv_obj_t *t2 = ui_pixel_label(panel,
            cfg->gh_user[0] ? ui_text(UI_T_GH_WAIT) : ui_text(UI_T_GH_NOUSER),
            &font_cjk_16, th->muted);
        lv_obj_set_pos(t2, 4, 44);
        lv_obj_t *t3 = ui_pixel_label(panel,
            cfg->gh_user[0] ? ui_text(UI_T_GH_WAIT_SUB) : ui_text(UI_T_GH_NOUSER_SUB),
            &font_cjk_16, th->muted);
        lv_obj_set_pos(t3, 4, 66);
        return;
    }
    if (!s_page_buf) {
        lv_obj_t *t2 = ui_pixel_label(panel, ui_text(UI_T_LOWMEM), &font_cjk_16, th->muted);
        lv_obj_set_pos(t2, 4, 44);
        return;
    }

    lv_obj_t *cv = lv_canvas_create(panel);
    lv_canvas_set_buffer(cv, s_page_buf, HEAT_W, HEAT_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, 4, 40);
    static const uint32_t COLORS[GH_LEVELS] = {
        0x2A323C, 0x9BE9A8, 0x56C271, 0x2EA043, 0x166DAB
    };
    lv_color_t cc[GH_LEVELS];
    for (int i = 0; i < GH_LEVELS; i++) cc[i] = lv_color_hex(COLORS[i]);
    // 逐格填 3x3 色块;先关失效避免每像素重复刷新,画完统一失效一次
    lv_display_t *disp = lv_obj_get_display(cv);
    lv_display_enable_invalidation(disp, false);
    for (int day = 0; day < GH_DAYS; day++) {
        int col = day / 7, row = day % 7;
        int level = app_github_level(day);
        lv_color_t c = cc[level < 0 ? 0 : level];
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                lv_canvas_set_px(cv, col * 4 + dx, row * 4 + dy, c, LV_OPA_COVER);
    }
    lv_display_enable_invalidation(disp, true);
    lv_obj_invalidate(cv);

    lv_obj_t *t4 = ui_pixel_label(panel, "", &font_cjk_16, th->ink);
    lv_obj_set_pos(t4, 4, 76);
    lv_label_set_text_fmt(t4, ui_text(UI_T_GH_TOTAL), app_github_total());
}

static void build_pet(const app_config_t *cfg, const ui_theme_t *th) {
    lv_obj_t *card = ui_pixel_panel_create(s_scr, 11, 52, 218, 178, th->panel);

    // 大号吉祥物:38x48 像素画,整数倍(2x)transform 缩放保持硬边
    s_mascot = ui_pixel_mascot_create(card, 79, 26);
    s_has_mascot = true;
    lv_obj_set_style_transform_scale(s_mascot, 512, 0);   // 256 = 1.0
    lv_obj_set_style_transform_pivot_x(s_mascot, 19, 0);
    lv_obj_set_style_transform_pivot_y(s_mascot, 24, 0);

    lv_obj_t *nm = ui_pixel_label(card, cfg->name, &font_cjk_20, th->ink);
    lv_obj_set_pos(nm, 0, 98);
    lv_obj_set_width(nm, 196);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *mood = ui_pixel_label(card, ui_text(UI_T_PET_MOOD), &font_cjk_16, th->muted);
    lv_obj_set_pos(mood, 0, 122);
    lv_obj_set_width(mood, 196);
    lv_obj_set_style_text_align(mood, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *soon = ui_pixel_label(card, ui_text(UI_T_PET_SOON), &font_cjk_16, th->muted);
    lv_obj_set_pos(soon, 0, 144);
    lv_obj_set_width(soon, 196);
    lv_obj_set_style_text_align(soon, LV_TEXT_ALIGN_CENTER, 0);
}

void demo_badge_enter(void) {
    const app_config_t *cfg = app_config_get();
    const ui_theme_t *th = ui_pixel_theme();
    s_has_mascot = false;
    s_gh_ready_at_enter = app_github_ready();
    s_page_buf = heap_caps_malloc(PAGE_BUF_MAX, MALLOC_CAP_8BIT);   // 失败则各布局降级
    s_scr = ui_pixel_screen_create("BADGE");

    switch (cfg->layout) {
    case APP_LAYOUT_QR:     build_qr(cfg, th);     break;
    case APP_LAYOUT_GITHUB: build_github(cfg, th); break;
    case APP_LAYOUT_PET:    build_pet(cfg, th);    break;
    default:                build_card(cfg, th);   break;
    }

    const ui_hint_t BADGE_HINTS[] = {
        { "OK",                        ui_text(UI_T_MENU),  false },
        { LV_SYMBOL_DOWN,              ui_text(UI_T_SLEEP), true  },
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_VIEW),  false },
    };
    ui_pixel_hints(s_scr, BADGE_HINTS, 3);
    if (cfg->layout == APP_LAYOUT_CARD) {
        // 只有名片布局显示时间;其他布局不需要秒级刷新
        clock_tick(NULL);
        s_timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, NULL);
    } else if (cfg->layout == APP_LAYOUT_GITHUB) {
        // 联网窗口拉到热力图数据时自动重绘
        s_timer = lv_timer_create(gh_tick, CLOCK_TICK_MS, NULL);
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
    }
    if (s_page_buf) { free(s_page_buf); s_page_buf = NULL; }
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
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        app_config_t c = *app_config_get();
        int dir = (btn == BSP_BTN_DOWN) ? +1 : -1;
        c.layout = app_config_next_layout(c.layout, c.layout_mask, dir);
        app_config_update(&c);
        demo_badge_exit();
        demo_badge_enter();
    }
}
