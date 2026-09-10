// main/demo_badge.c —— 工牌主页,三套布局,上/下短按循环切换:
//   A 名片   头像位(M2 换上传头像)+姓名/公司/岗位(默认隐藏)+时间
//   B 二维码 槽 A 文本(默认本项目主页 URL)设备端生成 QR
//   C GitHub 热力图占位(M4 在配网后拉取真实数据)
// 电量/音量/Wi-Fi 在每屏共用的顶部状态栏;自动息屏由 main.c 统一处理。
// 按键:OK 短按 = 打开菜单;▼ 长按 = 立即息屏;▲/▼ 短按 = 切换布局。
#include "demo.h"
#include "app_clock.h"
#include "app_config.h"
#include "ui_pixel.h"
#include "fonts/fonts.h"
#include "lvgl.h"
#include <string.h>

#define CLOCK_TICK_MS 1000
#define QR_SIZE       160       // QR 画布边长;LVGL 池已按此调到 48KB(sdkconfig.defaults)

static lv_obj_t *s_scr;
static lv_obj_t *s_name, *s_org, *s_title;
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_mascot, *s_qr;
static lv_timer_t *s_timer;
static bool s_has_mascot;

static void build_clock(const ui_theme_t *th, int y);

static void clock_tick(lv_timer_t *t) {
    (void)t;
    struct tm lt;
    static const char *WD[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    if (app_clock_local(&lt)) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d", lt.tm_hour, lt.tm_min);
        lv_label_set_text_fmt(s_date, "%02d-%02d %s", lt.tm_mon + 1, lt.tm_mday, WD[lt.tm_wday]);
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, "未校时");
    }
}

static lv_obj_t *text_line(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, int x, int y, int w) {
    lv_obj_t *label = ui_pixel_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);   // 超长姓名/岗位截断加省略号
    return label;
}

// 时间条:三套布局都保留,M2 接入 SNTP 后由 app_clock 每秒刷新
static void build_clock(const ui_theme_t *th, int y) {
    lv_obj_t *bar = ui_pixel_panel_create(s_scr, 11, y, 218, 52, th->panel);
    s_clock = ui_pixel_label(bar, "--:--", &lv_font_montserrat_28, th->ink);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);
    s_date = ui_pixel_label(bar, "未校时", &font_cjk_16, th->muted);
    lv_obj_align(s_date, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_card(const app_config_t *cfg, const ui_theme_t *th) {
    lv_obj_t *card = ui_pixel_panel_create(s_scr, 11, 52, 218, 118, th->panel);
    lv_obj_t *avatar = ui_pixel_panel_create(card, 0, 0, 64, 64, th->dim);
    s_mascot = ui_pixel_mascot_create(avatar, 6, 1);
    s_has_mascot = true;
    s_name  = text_line(card, cfg->name,  &font_cjk_20, th->ink,   78, 2,  126);
    s_org   = text_line(card, cfg->org,   &font_cjk_16, th->muted, 78, 32, 126);
    s_title = text_line(card, cfg->title, &font_cjk_16, th->muted, 78, 54, 126);
    if (cfg->hide_org_title) {
        lv_obj_add_flag(s_org, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    }
    build_clock(th, 182);
}

static void build_qr(const app_config_t *cfg, const ui_theme_t *th) {
    // 二维码固定黑白色保证扫码对比度;底部小字提示内容来源。此布局不显示时间。
    bool has = cfg->qr_a[0] != '\0';
    s_qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_qr, (240 - QR_SIZE) / 2, 62);
    if (has) lv_qrcode_update(s_qr, cfg->qr_a, strlen(cfg->qr_a));
    lv_obj_t *note = ui_pixel_label(s_scr, has ? "QR: 本地生成" : "QR: 未配置",
                                    &font_cjk_16, th->muted);
    lv_obj_set_pos(note, 0, 232);
    lv_obj_set_width(note, 240);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_github(const ui_theme_t *th) {
    // 占位:M4 在配网后拉取贡献热力图并缓存;当前明确告知状态,不画假数据。此布局不显示时间。
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 11, 52, 218, 190, th->panel);
    lv_obj_t *t1 = ui_pixel_label(panel, "GitHub 热力图", &font_cjk_20, th->ink);
    lv_obj_set_pos(t1, 4, 16);
    lv_obj_t *t2 = ui_pixel_label(panel, "等待网络连接…\n配网后自动拉取提交记录", &font_cjk_16, th->muted);
    lv_obj_set_pos(t2, 4, 60);
}

void demo_badge_enter(void) {
    const app_config_t *cfg = app_config_get();
    const ui_theme_t *th = ui_pixel_theme();
    s_has_mascot = false;
    s_scr = ui_pixel_screen_create("BADGE");

    switch (cfg->layout) {
    case APP_LAYOUT_QR:     build_qr(cfg, th);     break;
    case APP_LAYOUT_GITHUB: build_github(th);      break;
    default:                build_card(cfg, th);   break;
    }

    static const ui_hint_t BADGE_HINTS[] = {
        { "OK",                          "菜单", false },
        { LV_SYMBOL_DOWN,                "息屏", true  },
        { LV_SYMBOL_UP LV_SYMBOL_DOWN,   "布局", false },
    };
    ui_pixel_hints(s_scr, BADGE_HINTS, 3);
    if (cfg->layout == APP_LAYOUT_CARD) {
        // 只有名片布局显示时间;二维码/GitHub 布局不建时钟控件,也不需要秒级刷新
        clock_tick(NULL);                    // 进页立刻显示当前时间状态
        s_timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, NULL);
    }
    lv_screen_load(s_scr);
}

void demo_badge_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) {
        if (s_has_mascot) ui_pixel_mascot_stop(s_mascot);
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_name = s_org = s_title = s_clock = s_date = s_mascot = NULL;
    }
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
    // ▲/▼ 短按:循环切换布局,持久化后整页重建
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        app_config_t c = *app_config_get();
        c.layout = (uint8_t)((c.layout + APP_LAYOUT_COUNT + (btn == BSP_BTN_DOWN ? 1 : -1))
                             % APP_LAYOUT_COUNT);
        app_config_update(&c);
        demo_badge_exit();
        demo_badge_enter();
    }
}
