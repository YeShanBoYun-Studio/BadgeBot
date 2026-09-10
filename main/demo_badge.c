// main/demo_badge.c —— 工牌主页(布局 A:名片)。
//
// 内容:头像位(M2 换成上传头像,现为吉祥物占位)、姓名/公司/岗位(可隐藏)、时间(M2 接入 SNTP
// 前显示 --:--)。电量/音量/Wi-Fi 在每屏共用的顶部状态栏,自动息屏由 main.c 统一处理。
// 按键:OK 短按 = 打开菜单;▼ 长按 = 立即息屏;▲/▼ 短按 = 切换布局(M2 起有多套布局)。
#include "demo.h"
#include "app_clock.h"
#include "app_config.h"
#include "ui_pixel.h"
#include "fonts/fonts.h"
#include "lvgl.h"

#define CLOCK_TICK_MS 1000

static lv_obj_t *s_scr;
static lv_obj_t *s_name, *s_org, *s_title;
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_mascot;
static lv_timer_t *s_timer;

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

void demo_badge_enter(void) {
    const app_config_t *cfg = app_config_get();
    const ui_theme_t *th = ui_pixel_theme();
    s_scr = ui_pixel_screen_create("BADGE");

    // 资料卡:头像位 + 姓名 + 公司/岗位
    lv_obj_t *card = ui_pixel_panel_create(s_scr, 11, 52, 218, 118, th->panel);
    lv_obj_t *avatar = ui_pixel_panel_create(card, 0, 0, 64, 64, th->dim);
    s_mascot = ui_pixel_mascot_create(avatar, 6, 1);
    s_name  = text_line(card, cfg->name,  &font_cjk_20, th->ink,   78, 2,  126);
    s_org   = text_line(card, cfg->org,   &font_cjk_16, th->muted, 78, 32, 126);
    s_title = text_line(card, cfg->title, &font_cjk_16, th->muted, 78, 54, 126);
    if (cfg->hide_org_title) {
        lv_obj_add_flag(s_org, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    }

    // 时间条:M2 接入 SNTP 后由 app_clock 刷新
    lv_obj_t *bar = ui_pixel_panel_create(s_scr, 11, 182, 218, 52, th->panel);
    s_clock = ui_pixel_label(bar, "--:--", &lv_font_montserrat_28, th->ink);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);
    s_date = ui_pixel_label(bar, "未校时", &font_cjk_16, th->muted);
    lv_obj_align(s_date, LV_ALIGN_RIGHT_MID, 0, 0);

    static const ui_hint_t BADGE_HINTS[] = {
        { "OK",                          "菜单", false },
        { LV_SYMBOL_DOWN,                "息屏", true  },
        { LV_SYMBOL_UP LV_SYMBOL_DOWN,   "布局", false },
    };
    ui_pixel_hints(s_scr, BADGE_HINTS, 3);
    clock_tick(NULL);                        // 进页立刻显示当前时间状态
    s_timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_badge_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) {
        ui_pixel_mascot_stop(s_mascot);
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
    // ▲/▼ 短按:M2 起在名片/二维码/GitHub 布局间切换;M1 只有名片布局,仅反馈动画。
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        ui_pixel_mascot_jump(s_mascot);
    }
}
