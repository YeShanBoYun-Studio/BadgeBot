// main/demo_settings.c —— 设置页:亮度、音量、语言、主题、自动息屏、开机页、布局、恢复出厂。
//
// 操作:上/下选行;确定 单击=下一档、双击=上一档(button 组件保证单击与双击互斥)。
// "主页布局"行:单击进入布局开关子页(逐个开/关四套布局),双击直接切换默认布局。
// "恢复出厂"行:单击进入待确认(值显示"再按OK确认"),再单击执行;换行即取消。
//   执行后清除 NVS 配置+宠物、删除上传的头像/二维码图,主题立即回默认并重建本页。
// 每次改动立即通过 app_config_update() 生效并写入 NVS。
#include "demo.h"
#include "app_config.h"
#include "app_pet.h"
#include "app_store.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "lvgl.h"

enum {
    ROW_BRIGHTNESS,
    ROW_VOLUME,
    ROW_LANG,
    ROW_THEME,
    ROW_SCREEN_OFF,
    ROW_BOOT,
    ROW_LAYOUT,
    ROW_RESET,
    ROW_COUNT,
};

static lv_obj_t *s_scr;
static lv_obj_t *s_cards[ROW_COUNT];
static lv_obj_t *s_values[ROW_COUNT];
static int s_sel;
static bool s_edit_layouts;          // true = 正在布局开关子页
static int s_lay_sel;                // 子页选中行(app_layout_t)
static bool s_reset_arm;             // true = 恢复出厂已待确认,再按 OK 执行
static app_config_t s_cfg;           // 页面内的工作副本,改动后整体提交

static const char *lang_text(ui_str_id id) {
    return ui_text_lang(s_cfg.lang, id);
}

static const char *layout_label(uint8_t layout) {
    switch (layout) {
    case APP_LAYOUT_CARD:   return lang_text(UI_T_LAY_CARD);
    case APP_LAYOUT_QR:     return lang_text(UI_T_LAY_QR);
    case APP_LAYOUT_PET:    return lang_text(UI_T_LAY_PET);
    default:                return "?";
    }
}

// ---- 主设置页 ----

static void refresh(void) {
    for (int i = 0; i < ROW_COUNT; i++) {
        ui_pixel_set_selected(s_cards[i], i == s_sel, true);
    }
    lv_label_set_text_fmt(s_values[ROW_BRIGHTNESS], "%d%%", s_cfg.brightness);
    lv_label_set_text_fmt(s_values[ROW_VOLUME], "%d%%", s_cfg.volume);
    lv_label_set_text(s_values[ROW_LANG],
                      s_cfg.lang == 0 ? lang_text(UI_T_LANG_ZH) : lang_text(UI_T_LANG_EN));
    lv_label_set_text(s_values[ROW_THEME],
                      s_cfg.theme == 1 ? lang_text(UI_T_TH_DARK) : lang_text(UI_T_TH_LIGHT));
    if (s_cfg.screen_off_min == 0) {
        lv_label_set_text(s_values[ROW_SCREEN_OFF], lang_text(UI_T_OFF_ALWAYS));
    } else {
        lv_label_set_text_fmt(s_values[ROW_SCREEN_OFF], lang_text(UI_T_OFF_MIN),
                              s_cfg.screen_off_min);
    }
    lv_label_set_text(s_values[ROW_BOOT],
                      s_cfg.boot_badge ? lang_text(UI_T_BOOT_BADGE) : lang_text(UI_T_BOOT_MENU));
    lv_label_set_text(s_values[ROW_LAYOUT],
                      s_cfg.layout < APP_LAYOUT_COUNT ? layout_label(s_cfg.layout) : "?");
    // 恢复出产行:平时空值,待确认时红色高亮提示
    if (s_reset_arm) {
        lv_label_set_text(s_values[ROW_RESET], lang_text(UI_T_RESET_ARM));
        lv_obj_set_style_text_color(s_values[ROW_RESET],
                                    lv_color_hex(ui_pixel_theme()->accent), 0);
    } else {
        lv_label_set_text(s_values[ROW_RESET], "");
    }
}

static void build_rows(void) {
    static const ui_str_id ROWS[ROW_COUNT] = {
        UI_T_ROW_BL, UI_T_ROW_VOL, UI_T_ROW_LANG, UI_T_ROW_THEME,
        UI_T_ROW_OFF, UI_T_ROW_BOOT, UI_T_ROW_LAYOUT, UI_T_ROW_RESET,
    };
    for (int i = 0; i < ROW_COUNT; i++) {
        s_cards[i] = ui_pixel_panel_create(s_scr, 11, 52 + i * 29, 218, 26,
                                           ui_pixel_theme()->panel);
        lv_obj_t *name = ui_pixel_label(s_cards[i], ui_text(ROWS[i]), &font_cjk_16,
                                        ui_pixel_theme()->ink);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_pixel_label(s_cards[i], "", &font_cjk_16,
                                     ui_pixel_theme()->ink);
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void adjust(int dir) {
    switch (s_sel) {
    case ROW_BRIGHTNESS:
        s_cfg.brightness = app_config_step_brightness(s_cfg.brightness, dir);
        break;
    case ROW_VOLUME:
        s_cfg.volume = app_config_step_volume(s_cfg.volume, dir);
        break;
    case ROW_LANG:
        s_cfg.lang = (uint8_t)((s_cfg.lang + 2 + dir) % 2);
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
    case ROW_LAYOUT:
        // 双击 = 直接切换默认布局(全部布局参与,不受开关限制)
        s_cfg.layout = (uint8_t)((s_cfg.layout + APP_LAYOUT_COUNT + dir) % APP_LAYOUT_COUNT);
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
    if (s_sel == ROW_LANG) {
        // 语言影响本页所有行名:重建本页并停留在语言行
        int keep = s_sel;
        demo_settings_exit();
        demo_settings_enter();
        s_sel = keep;
        refresh();
        return;
    }
    refresh();
}

// ---- 布局开关子页 ----

static void lay_refresh(void) {
    for (int i = 0; i < APP_LAYOUT_COUNT; i++) {
        ui_pixel_set_selected(s_cards[i], i == s_lay_sel, true);
        lv_label_set_text(s_values[i],
                          (s_cfg.layout_mask & (1 << i)) ? lang_text(UI_T_ON)
                                                         : lang_text(UI_T_OFF));
    }
}

static void build_layout_page(void) {
    s_scr = ui_pixel_screen_create("LAYOUT");
    for (int i = 0; i < APP_LAYOUT_COUNT; i++) {
        s_cards[i] = ui_pixel_panel_create(s_scr, 11, 64 + i * 40, 218, 33,
                                           ui_pixel_theme()->panel);
        lv_obj_t *name = ui_pixel_label(s_cards[i], layout_label((uint8_t)i), &font_cjk_16,
                                        ui_pixel_theme()->ink);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_pixel_label(s_cards[i], "", &font_cjk_16,
                                     ui_pixel_theme()->ink);
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    const ui_hint_t HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_SEL),    false },
        { "OK",                        ui_text(UI_T_TOGGLE), false },
        { "OK x2",                     ui_text(UI_T_DONE),   false },
    };
    ui_pixel_hints(s_scr, HINTS, 3);
    lay_refresh();
    lv_screen_load(s_scr);
}

// 至少保留一个开启的布局;关掉最后一个时拒绝(值标签不动,即反馈)
static bool mask_toggle(uint8_t bit) {
    uint8_t mask = s_cfg.layout_mask;
    if (mask & (1 << bit)) {
        if (mask == (uint8_t)(1 << bit)) return false;
        mask &= (uint8_t)~(1 << bit);
    } else {
        mask |= (uint8_t)(1 << bit);
    }
    s_cfg.layout_mask = mask;
    app_config_update(&s_cfg);
    s_cfg = *app_config_get();
    return true;
}

// ---- 恢复出厂 ----

// 擦 NVS(配置+宠物)、删上传资产,主题回默认后重建本页
static void do_factory_reset(void) {
    app_config_factory_reset();
    app_pet_reset();
    if (app_store_ready()) {
        app_store_write("avatar.rgb565", NULL, 0);
        for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
            char name[12];
            snprintf(name, sizeof(name), "qr%d.img", i);
            app_store_write(name, NULL, 0);
        }
        app_store_write("qr_b.img", NULL, 0);   // 旧版单二维码文件一并清理
        app_store_write("notes.txt", NULL, 0);  // 讲稿同属用户上传内容
    }
    s_cfg = *app_config_get();
    ui_pixel_set_theme(s_cfg.theme);
    int keep = s_sel;
    demo_settings_exit();
    demo_settings_enter();
    s_sel = keep;
    refresh();
}

void demo_settings_enter(void) {
    s_cfg = *app_config_get();
    s_sel = 0;
    s_lay_sel = 0;
    s_edit_layouts = false;
    s_reset_arm = false;
    s_scr = ui_pixel_screen_create("SETTINGS");
    build_rows();
    const ui_hint_t SET_HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_SEL), false },
        { "OK",                        ui_text(UI_T_ADJ), false },
        { "OK x2",                     ui_text(UI_T_BACK), false },
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
        for (int i = 0; i < APP_LAYOUT_COUNT; i++) s_cards[i] = s_values[i] = NULL;
    }
    s_edit_layouts = false;
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK) {
        if (s_edit_layouts) {
            if (ev == BSP_BTN_CLICK) {
                mask_toggle((uint8_t)s_lay_sel);
                lay_refresh();
            } else if (ev == BSP_BTN_DOUBLE) {
                demo_settings_exit();
                demo_settings_enter();       // 回主设置页
            }
            return;
        }
        if (ev == BSP_BTN_CLICK) {
            if (s_sel == ROW_LAYOUT) {       // 单击 = 进入布局开关子页
                demo_settings_exit();
                s_edit_layouts = true;
                s_lay_sel = 0;
                build_layout_page();
            } else if (s_sel == ROW_RESET) { // 单击 = 待确认;再单击 = 执行
                if (s_reset_arm) {
                    do_factory_reset();
                } else {
                    s_reset_arm = true;
                    refresh();
                }
            } else {
                adjust(+1);
            }
        }
        if (ev == BSP_BTN_DOUBLE) adjust(-1);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (s_edit_layouts) {
        s_lay_sel = (btn == BSP_BTN_UP) ? (s_lay_sel + APP_LAYOUT_COUNT - 1) % APP_LAYOUT_COUNT
                                        : (s_lay_sel + 1) % APP_LAYOUT_COUNT;
        lay_refresh();
        return;
    }
    s_sel = (btn == BSP_BTN_UP) ? (s_sel + ROW_COUNT - 1) % ROW_COUNT
                                : (s_sel + 1) % ROW_COUNT;
    s_reset_arm = false;                 // 换行即取消待确认
    refresh();
}
