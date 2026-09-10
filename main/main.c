// main/main.c —— FoloToy AI Passport:初始化 + 菜单 + 按键分发 + 全局息屏与状态栏刷新。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项(越过页尾自动翻页);演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
//   息屏中任意键 = 亮屏,这一次按键不再分发
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "app_config.h"
#include "app_github.h"
#include "app_store.h"
#include "app_wifi.h"
#include "demo.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 枚举顺序即菜单顺序;DEMOS[] 与 s_ok[] 都按它索引,新增页面只需在此加一项并补 DEMOS 行。
enum {
    DEMO_BADGE,
    DEMO_SETTINGS,
    DEMO_PORTAL,
    DEMO_DISPLAY,
    DEMO_BUTTON,
    DEMO_AUDIO,
    DEMO_BATTERY,
    DEMO_BLE,
    DEMO_LOW_POWER,
    DEMO_COUNT,
};

static const demo_entry_t DEMOS[DEMO_COUNT] = {
    [DEMO_BADGE]     = { "Badge",     "工牌",   demo_badge_enter,     demo_badge_exit,     demo_badge_key     },
    [DEMO_SETTINGS]  = { "Settings",  "设置",   demo_settings_enter,  demo_settings_exit,  demo_settings_key  },
    [DEMO_PORTAL]    = { "Portal",    "配网",   demo_portal_enter,    demo_portal_exit,    demo_portal_key    },
    [DEMO_DISPLAY]   = { "Display",   "屏幕",   demo_display_enter,   demo_display_exit,   demo_display_key   },
    [DEMO_BUTTON]    = { "Button",    "按键",   demo_button_enter,    demo_button_exit,    demo_button_key    },
    [DEMO_AUDIO]     = { "Audio",     "音频",   demo_audio_enter,     demo_audio_exit,     demo_audio_key     },
    [DEMO_BATTERY]   = { "Battery",   "电池",   demo_battery_enter,   demo_battery_exit,   demo_battery_key   },
    [DEMO_BLE]       = { "BLE",       "蓝牙",   demo_ble_enter,       demo_ble_exit,       demo_ble_key       },
    [DEMO_LOW_POWER] = { "Low Power", "低功耗", demo_low_power_enter, demo_low_power_exit, demo_low_power_key },
};

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

// 菜单每页 2 列 × 4 行。卡位在建屏时一次建满,翻页只改文字与可见性,
// 不删屏重建 —— 屏上有无限循环的眨眼动画,反复建删容易留下悬空动画。
#define MENU_PAGE_SIZE 8

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[MENU_PAGE_SIZE];
static lv_obj_t *s_shadows[MENU_PAGE_SIZE];   // 卡片投影,和卡片一起显示/隐藏
static lv_obj_t *s_rows[MENU_PAGE_SIZE];
static lv_obj_t *s_page_label;
static lv_obj_t *s_mascot;
static int  s_sel;                 // 当前选中项(全局下标)
static int  s_page;                // 当前显示的菜单页
static int  s_active = -1;         // 当前所在演示页;-1 = 在菜单

// 全局息屏与状态栏:1 秒节拍。息屏计时只看按键,任意键亮屏。
#define SYS_TICK_MS        1000
#define STATUS_EVERY_TICKS 2       // 状态栏每 2 秒刷新(电量计走 I2C,不必更频繁)
static lv_timer_t *s_sys_timer;
static uint32_t s_last_key_tick;
static bool     s_screen_off;
static bool     s_swallow;         // 唤醒屏幕的那次按键,其后续 CLICK/LONG 不分发
static int      s_sys_ticks;

static int page_count(void) { return (DEMO_COUNT + MENU_PAGE_SIZE - 1) / MENU_PAGE_SIZE; }

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void menu_refresh(void) {
    s_page = s_sel / MENU_PAGE_SIZE;
    int first = s_page * MENU_PAGE_SIZE;
    for (int i = 0; i < MENU_PAGE_SIZE; i++) {
        int idx = first + i;
        bool present = idx < DEMO_COUNT;
        set_hidden(s_cards[i], !present);
        set_hidden(s_shadows[i], !present);
        if (!present) continue;
        const char *nm = DEMOS[idx].name;
        if (app_config_get()->lang == 0 && DEMOS[idx].name_zh) nm = DEMOS[idx].name_zh;
        lv_label_set_text_fmt(s_rows[i], "%s%s", nm, s_ok[idx] ? "" : "  [FAIL]");
        ui_pixel_set_selected(s_cards[i], idx == s_sel, s_ok[idx]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[idx] ? lv_color_hex(ui_pixel_theme()->ink) : lv_color_hex(UI_RED), 0);
    }
    if (s_page_label) {
        lv_label_set_text_fmt(s_page_label, "%d/%d", s_page + 1, page_count());
    }
}

static void menu_build(void) {
    // 产品名 BadgeBot:菜单标题、开机日志、配网热点名(M2)统一使用。
    s_menu_scr = ui_pixel_screen_create("BADGEBOT");

    for (int i = 0; i < MENU_PAGE_SIZE; i++) {
        int x = 11 + (i % 2) * 112;
        int y = 52 + (i / 2) * 47;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, 102, 40,
                                           ui_pixel_theme()->panel);
        // ui_pixel_panel_create 先建投影再建卡片,两者相邻,投影就是前一个子对象。
        s_shadows[i] = lv_obj_get_child(s_menu_scr, lv_obj_get_index(s_cards[i]) - 1);
        s_rows[i] = lv_label_create(s_cards[i]);
        // 行名可含中文(如"配网"),用 font_cjk_16;图标字形回退到 Montserrat
        lv_obj_set_style_text_font(s_rows[i], &font_cjk_16, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 238);
    s_page_label = NULL;
    if (page_count() > 1) {
        s_page_label = ui_pixel_label(s_menu_scr, "", &lv_font_montserrat_14, UI_PAPER);
        lv_obj_set_pos(s_page_label, 200, 262);
    }
    const ui_hint_t MENU_HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_SEL),  false },
        { "OK",                        ui_text(UI_T_OPEN), false },
    };
    ui_pixel_hints(s_menu_scr, MENU_HINTS, 2);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

static void menu_select(int sel) {
    s_sel = sel;
    menu_refresh();
    ui_pixel_mascot_jump(s_mascot);
}

static void enter_demo(int idx) {
    s_active = idx;
    if (s_menu_scr) {
        ui_pixel_mascot_stop(s_mascot);
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
        s_mascot = NULL;
        s_page_label = NULL;
    }
    DEMOS[s_active].enter();
}

void demo_request_menu(void) {
    if (s_active < 0) return;
    DEMOS[s_active].exit();
    enter_menu();
}

// Display 页自己调背光档位,Low Power 页自己睡眠,这两页不做自动息屏。
static bool page_manages_backlight(void) {
    return s_active == DEMO_DISPLAY || s_active == DEMO_LOW_POWER;
}

static void screen_off(void) {
    if (s_screen_off) return;
    bsp_display_backlight(0);
    s_screen_off = true;
}

static void screen_on(void) {
    s_screen_off = false;
    bsp_display_backlight(app_config_get()->brightness);
}

void demo_request_screen_off(void) {
    screen_off();
}

static void status_refresh(void) {
    ui_pixel_status_update(bsp_battery_soc(), app_config_get()->volume,
                           app_wifi_connected(), false);
}

static void sys_tick(lv_timer_t *t) {
    (void)t;
    if (++s_sys_ticks % STATUS_EVERY_TICKS == 0) status_refresh();

    const app_config_t *cfg = app_config_get();
    if (!s_screen_off && cfg->screen_off_min > 0 && !page_manages_backlight() &&
        lv_tick_elaps(s_last_key_tick) >= (uint32_t)cfg->screen_off_min * 60000u) {
        screen_off();
    }
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    s_last_key_tick = lv_tick_get();

    // 每次物理按下都以 PRESS 开头:息屏时它只负责亮屏,并让同一次按下的后续事件作废。
    if (ev == BSP_BTN_PRESS) {
        if (s_screen_off) { screen_on(); s_swallow = true; ESP_LOGI(TAG, "唤醒亮屏"); bsp_lvgl_unlock(); return; }
        s_swallow = false;
    } else if (s_swallow) {
        ESP_LOGI(TAG, "唤醒键后续事件不分发 btn=%d ev=%d", btn, ev);
        bsp_lvgl_unlock();
        return;
    }

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {     // 统一返回
            ESP_LOGI(TAG, "OK 长按 -> 返回菜单");
            demo_request_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK && s_menu_scr) {        // 菜单尚未建好时忽略按键
        if (btn == BSP_BTN_UP)   menu_select((s_sel + DEMO_COUNT - 1) % DEMO_COUNT);
        if (btn == BSP_BTN_DOWN) menu_select((s_sel + 1) % DEMO_COUNT);
        if (btn == BSP_BTN_OK && s_ok[s_sel]) enter_demo(s_sel);
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "BadgeBot 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    // 配置先于显示:背光初值、主题与开机页都取自 NVS。
    app_config_init();
    app_store_init();          // 上传资产(头像/二维码图)的 FATFS,失败仅降级相关功能
    app_github_load();         // 热力图缓存(有则主页布局 C 立即可画)
    ui_pixel_set_theme(app_config_get()->theme);

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出,不做"串口菜单"降级。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(app_config_get()->brightness);

    // 其余外设单项失败不阻塞:菜单里标 [FAIL],其他项照常可用。
    s_ok[DEMO_BADGE]     = true;                              // 只依赖显示
    s_ok[DEMO_SETTINGS]  = true;
    s_ok[DEMO_PORTAL]    = true;
    s_ok[DEMO_DISPLAY]   = true;
    s_ok[DEMO_BUTTON]    = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[DEMO_AUDIO]     = (bsp_audio_init() == ESP_OK);
    s_ok[DEMO_BATTERY]   = (bsp_battery_init() == ESP_OK);
    s_ok[DEMO_BLE]       = true;
    s_ok[DEMO_LOW_POWER] = true;
    app_config_apply();                                       // 音量要等 audio 初始化后再设

    // Wi-Fi 脉冲:每小时连一次做校时/拉取,凭据用设备里已存的(官方固件配过网就有)。
    app_wifi_start();

    if (bsp_lvgl_lock(1000)) {
        s_last_key_tick = lv_tick_get();
        status_refresh();                                     // 首屏建好时状态栏就有值
        if (app_config_get()->boot_badge) enter_demo(DEMO_BADGE);
        else                              enter_menu();
        s_sys_timer = lv_timer_create(sys_tick, SYS_TICK_MS, NULL);
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             s_ok[DEMO_DISPLAY], s_ok[DEMO_BUTTON], s_ok[DEMO_AUDIO], s_ok[DEMO_BATTERY]);
}
