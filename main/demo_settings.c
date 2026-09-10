// main/demo_settings.c —— 设置页:亮度、音量、主页布局、主题、自动息屏、开机页。
//
// 操作:上/下选行;确定 单击=下一档、双击=上一档(button 组件保证单击与双击互斥)。
// 每次改动立即通过 app_config_update() 生效并写入 NVS;主题切换后本页立即重建生效。
#include "demo.h"
#include "app_config.h"
#include "ui_pixel.h"
#include "fonts/fonts.h"
#include "lvgl.h"

enum {
    ROW_BRIGHTNESS,
    ROW_VOLUME,
    ROW_LAYOUT,
    ROW_THEME,
    ROW_SCREEN_OFF,
    ROW_BOOT,
    ROW_ABOUT,
    ROW_COUNT,
};

static const char *ROW_NAME[ROW_COUNT] = {
    "亮度", "音量", "主页布局", "主题", "自动息屏", "开机页面", "固件版本",
};

// 布局的显示名(app_config_layout_name 返回的英文名用于日志与测试)。
static const char *LAYOUT_LABEL[APP_LAYOUT_COUNT] = { "名片", "二维码", "GitHub" };

static lv_obj_t *s_scr;
static lv_obj_t *s_cards[ROW_COUNT];
static lv_obj_t *s_values[ROW_COUNT];
static int s_sel;
static app_config_t s_cfg;      // 页面内的工作副本,改动后整体提交

static void refresh(void) {
    for (int i = 0; i < ROW_COUNT; i++) {
        ui_pixel_set_selected(s_cards[i], i == s_sel, true);
    }
    lv_label_set_text_fmt(s_values[ROW_BRIGHTNESS], "%d%%", s_cfg.brightness);
    lv_label_set_text_fmt(s_values[ROW_VOLUME], "%d%%", s_cfg.volume);
    lv_label_set_text(s_values[ROW_LAYOUT],
                      s_cfg.layout < APP_LAYOUT_COUNT ? LAYOUT_LABEL[s_cfg.layout] : "?");
    lv_label_set_text(s_values[ROW_THEME], s_cfg.theme == 1 ? "深色" : "浅色");
    if (s_cfg.screen_off_min == 0) {
        lv_label_set_text(s_values[ROW_SCREEN_OFF], "常亮");
    } else {
        lv_label_set_text_fmt(s_values[ROW_SCREEN_OFF], "%d 分钟", s_cfg.screen_off_min);
    }
    lv_label_set_text(s_values[ROW_BOOT], s_cfg.boot_badge ? "主页" : "菜单");
    lv_label_set_text(s_values[ROW_ABOUT], "M1.5");
}

static void adjust(int dir) {
    switch (s_sel) {
    case ROW_BRIGHTNESS:
        s_cfg.brightness = app_config_step_brightness(s_cfg.brightness, dir);
        break;
    case ROW_VOLUME:
        s_cfg.volume = app_config_step_volume(s_cfg.volume, dir);
        break;
    case ROW_LAYOUT:
        s_cfg.layout = (uint8_t)((s_cfg.layout + APP_LAYOUT_COUNT + dir) % APP_LAYOUT_COUNT);
        break;
    case ROW_THEME:
        s_cfg.theme = (uint8_t)((s_cfg.theme + UI_THEME_COUNT + dir) % UI_THEME_COUNT);
        break;
    case ROW_SCREEN_OFF:
        s_cfg.screen_off_min = app_config_step_screen_off(s_cfg.screen_off_min, dir);
        break;
    case ROW_BOOT:
        s_cfg.boot_badge = !s_cfg.boot_badge;
        break;
    default:
        return;
    }
    app_config_update(&s_cfg);
    s_cfg = *app_config_get();       // 取回经校验的值,保持副本与生效配置一致

    if (s_sel == ROW_THEME) {
        // 主题作用于"新建的屏":同步到 ui_pixel 后重建本页,立即看到效果并停留在主题行
        ui_pixel_set_theme(s_cfg.theme);
        int keep = s_sel;
        demo_settings_exit();
        demo_settings_enter();
        s_sel = keep;
        refresh();
        return;
    }
    refresh();
}

void demo_settings_enter(void) {
    s_cfg = *app_config_get();
    s_sel = 0;
    s_scr = ui_pixel_screen_create("SETTINGS");

    // 7 行:52 + 6*33 + 29 = 279,压在底部条上方
    for (int i = 0; i < ROW_COUNT; i++) {
        s_cards[i] = ui_pixel_panel_create(s_scr, 11, 52 + i * 33, 218, 29,
                                           ui_pixel_theme()->panel);
        lv_obj_t *name = ui_pixel_label(s_cards[i], ROW_NAME[i], &font_cjk_16,
                                        ui_pixel_theme()->ink);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_pixel_label(s_cards[i], "", &font_cjk_16,
                                     ui_pixel_theme()->ink);
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    static const ui_hint_t SET_HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, "选择", false },
        { "OK",                        "调整", false },
        { "OK x2",                     "回退", false },
    };
    ui_pixel_hints(s_scr, SET_HINTS, 3);

    refresh();
    lv_screen_load(s_scr);
}

void demo_settings_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        for (int i = 0; i < ROW_COUNT; i++) s_cards[i] = s_values[i] = NULL;
    }
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK)  adjust(+1);
        if (ev == BSP_BTN_DOUBLE) adjust(-1);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    s_sel = (btn == BSP_BTN_UP) ? (s_sel + ROW_COUNT - 1) % ROW_COUNT
                                : (s_sel + 1) % ROW_COUNT;
    refresh();
}
