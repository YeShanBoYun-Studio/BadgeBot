// main/demo_hid.c —— BLE 翻页器页:进入即以蓝牙键盘广播,电脑配对后 ▲/▼ 发翻页键。
//
// 按键:上/下 短按 = 上一页/下一页(按当前键位映射);确定 短按 = 切换键位映射
//      (PgUp/PgDn ↔ 左/右方向键,NVS 持久化);确定 长按 = 返回菜单(全局拦截)。
// 页面用 250ms 定时轮询连接状态(广播中/已连接/已就绪),无需跨任务加锁。
#include "demo.h"
#include "app_blehid.h"
#include "app_blehid_keys.h"
#include "app_config.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "esp_log.h"
#include "lvgl.h"

#define HID_TICK_MS 250

static const char *TAG = "demo_hid";

static lv_obj_t *s_scr;
static lv_obj_t *s_state_label;     // 大字连接状态(居中)
static lv_obj_t *s_map_label;       // 当前键位映射(置灰)
static lv_timer_t *s_timer;

static void refresh_state(void) {
    switch (app_blehid_state()) {
    case BLEHID_ADVERTISING:
        lv_label_set_text(s_state_label, ui_text(UI_T_HID_WAIT));
        lv_obj_set_style_text_color(s_state_label,
                                    lv_color_hex(ui_pixel_theme()->muted), 0);
        break;
    case BLEHID_CONNECTED:
        lv_label_set_text(s_state_label, ui_text(UI_T_HID_CONN));
        lv_obj_set_style_text_color(s_state_label,
                                    lv_color_hex(ui_pixel_theme()->ink), 0);
        break;
    case BLEHID_READY:
        lv_label_set_text(s_state_label, ui_text(UI_T_HID_READY));
        lv_obj_set_style_text_color(s_state_label,
                                    lv_color_hex(ui_pixel_theme()->accent), 0);
        break;
    default:
        lv_label_set_text(s_state_label, "BLE failed");
        lv_obj_set_style_text_color(s_state_label,
                                    lv_color_hex(UI_RED), 0);
        break;
    }
}

static void refresh_map(void) {
    uint8_t map = app_config_get()->hid_arrows;
    lv_label_set_text(s_map_label, map == APP_BLEHID_MAP_ARROWS
                                       ? ui_text(UI_T_HID_MAP_AR)
                                       : ui_text(UI_T_HID_MAP_PG));
}

static void tick(lv_timer_t *t) {
    (void)t;
    refresh_state();
}

void demo_hid_enter(void) {
    s_scr = ui_pixel_screen_create("TURNER");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 11, 52, 218, 120,
                                            ui_pixel_theme()->panel);
    s_state_label = ui_pixel_label(panel, ui_text(UI_T_HID_WAIT), &font_cjk_20,
                                   ui_pixel_theme()->muted);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 24);
    s_map_label = ui_pixel_label(panel, "", &font_cjk_16, ui_pixel_theme()->muted);
    lv_obj_align(s_map_label, LV_ALIGN_TOP_MID, 0, 74);

    const ui_hint_t HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_SEND),   false },
        { "OK",                        ui_text(UI_T_KEYMAP), false },
        { "OK",                        ui_text(UI_T_BACK),   true  },
    };
    ui_pixel_hints(s_scr, HINTS, 3);

    refresh_map();
    refresh_state();
    s_timer = lv_timer_create(tick, HID_TICK_MS, NULL);
    lv_screen_load(s_scr);

    esp_err_t err = app_blehid_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 启动失败 %s", esp_err_to_name(err));
    }
    refresh_state();
}

void demo_hid_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    app_blehid_shutdown();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_state_label = s_map_label = NULL;
    }
}

void demo_hid_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;                 // OK 长按已被 main.c 拦截回菜单
    if (btn == BSP_BTN_UP) {
        app_blehid_send(-1);
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        app_blehid_send(+1);
        return;
    }
    if (btn == BSP_BTN_OK) {                          // 切换键位映射并持久化
        app_config_t c = *app_config_get();
        c.hid_arrows = (c.hid_arrows == APP_BLEHID_MAP_ARROWS)
                           ? APP_BLEHID_MAP_PAGE
                           : APP_BLEHID_MAP_ARROWS;
        app_config_update(&c);
        refresh_map();
    }
}
