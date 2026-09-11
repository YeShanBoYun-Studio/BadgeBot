// main/demo_prompter.c —— 提词器页(M5):讲稿来自门户「提词稿」(notes.txt,一行 = 一段),
// ▲/▼ 翻段的同时经 BLE HID 给电脑发翻页键,PPT 与提词稿同步,全程电脑零软件。
//
// 两种模式(OK 短按切换):
//   联动 —— 对着 PPT 讲:▲/▼ = 上/下一页(HID + 翻段);段长超屏时缓慢滚动补看。
//   自动 —— 纯演讲:按当前速度连续滚动,一段滚完自动进下一段;
//           ▲/▼ 调速,OK 双击暂停/继续。
// 进页时挂起 Wi-Fi 起蓝牙(与翻页器同一互斥机制),退页恢复。
#include "demo.h"
#include "app_blehid.h"
#include "app_prompter_model.h"
#include "app_store.h"
#include "app_config.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTES_MAX       32768     // 讲稿字节上限(与门户上传上限一致)
#define PROMPT_TICK_MS  100
#define SPEED_MAX       12        // 自动模式滚动速度(px/拍),1 拍 = 100ms
#define SPEED_DEFAULT   3

static const char *TAG = "demo_prompter";

static char *s_text;                       // 整篇讲稿(('\n' 分行,NUL 结尾)
static char *s_line;                       // 当前段的显示副本
static lv_obj_t *s_scr, *s_progress, *s_mode_lbl, *s_view, *s_body;
static lv_timer_t *s_timer;
static int s_seg, s_count;
static bool s_auto;                        // false = 联动,true = 自动滚动
static bool s_paused;
static int s_speed = SPEED_DEFAULT;
static int s_offset;                       // 当前段内已滚动像素

static void refresh_mode(void) {
    const ui_theme_t *th = ui_pixel_theme();
    char buf[24];
    if (s_auto) {
        snprintf(buf, sizeof(buf), "%s%s", ui_text(UI_T_MODE_AUTO),
                 s_paused ? ui_text(UI_T_PAUSED) : "");
        lv_label_set_text(s_mode_lbl, buf);
        lv_obj_set_style_text_color(s_mode_lbl,
                                    lv_color_hex(s_paused ? th->muted : th->accent), 0);
    } else {
        lv_label_set_text(s_mode_lbl, ui_text(UI_T_MODE_LK));
        lv_obj_set_style_text_color(s_mode_lbl, lv_color_hex(th->ink), 0);
    }
}

static void show_segment(void) {
    if (s_count == 0 || !s_line) return;
    prompter_segment(s_text, s_seg, s_line, prompter_max_line_len(s_text) + 1);
    lv_label_set_text(s_body, s_line);
    s_offset = 0;
    lv_obj_scroll_to_y(s_view, 0, LV_ANIM_OFF);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", s_seg + 1, s_count);
    lv_label_set_text(s_progress, buf);
}

// 每 100ms:自动模式下推进滚动,一段到底自动进下一段
static void prompt_tick(lv_timer_t *t) {
    (void)t;
    if (!s_auto || s_paused || s_count == 0) return;
    int max_off = (int)lv_obj_get_height(s_body) - (int)lv_obj_get_height(s_view);
    if (max_off <= 0) {                    // 本段一屏放得下:直接翻段
        s_seg = (s_seg + 1) % s_count;
        show_segment();
        return;
    }
    if (s_offset >= max_off) {
        s_seg = (s_seg + 1) % s_count;
        show_segment();
        return;
    }
    s_offset += s_speed;
    if (s_offset > max_off) s_offset = max_off;
    lv_obj_scroll_to_y(s_view, s_offset, LV_ANIM_OFF);
}

void demo_prompter_enter(void) {
    // 读讲稿:按实际文件大小精确分配(典型几 KB,上限 32KB)
    long size = app_store_size("notes.txt");
    if (size > NOTES_MAX) size = NOTES_MAX;
    if (size > 0) {
        s_text = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_8BIT);
        if (s_text) {
            int n = app_store_read("notes.txt", s_text, (size_t)size);
            if (n > 0) {
                s_text[n] = '\0';
                s_count = prompter_segment_count(s_text);
            } else {
                free(s_text);
                s_text = NULL;
            }
        } else {
            ESP_LOGW(TAG, "讲稿缓冲分配失败(%ld 字节),按无讲稿处理", size);
        }
    }

    s_scr = ui_pixel_screen_create("NOTES");

    if (s_count == 0) {
        lv_obj_t *ph = ui_pixel_label(s_scr, ui_text(UI_T_NO_NOTES), &font_cjk_16,
                                      ui_pixel_theme()->muted);
        lv_obj_center(ph);
        const ui_hint_t HINTS[] = {
            { "OK", ui_text(UI_T_BACK), true },
        };
        ui_pixel_hints(s_scr, HINTS, 1);
        lv_screen_load(s_scr);
        return;
    }

    // 信息行:段进度(左) + 模式(右)
    s_progress = ui_pixel_label(s_scr, "", &lv_font_montserrat_16, ui_pixel_theme()->ink);
    lv_obj_set_pos(s_progress, 12, 30);
    s_mode_lbl = ui_pixel_label(s_scr, "", &font_cjk_16, ui_pixel_theme()->ink);
    lv_obj_set_pos(s_mode_lbl, 150, 28);

    // 滚动视口:面板内一个宽 208 的标签(20px 字,约 11 个汉字/行)
    s_view = ui_pixel_panel_create(s_scr, 8, 52, 224, 228, ui_pixel_theme()->panel);
    s_body = ui_pixel_label(s_view, "", &font_cjk_20, ui_pixel_theme()->ink);
    lv_obj_set_pos(s_body, 8, 6);
    lv_obj_set_width(s_body, 208);

    size_t line_cap = prompter_max_line_len(s_text) + 1;
    s_line = heap_caps_malloc(line_cap, MALLOC_CAP_8BIT);
    if (!s_line) {                          // 显示缓冲不足:退化为小缓冲逐段截断
        line_cap = 256;
        s_line = heap_caps_malloc(line_cap, MALLOC_CAP_8BIT);
    }

    const ui_hint_t HINTS[] = {
        { LV_SYMBOL_UP LV_SYMBOL_DOWN, ui_text(UI_T_TURN),  false },
        { "OK",                        ui_text(UI_T_MODE),  false },
        { "OK x2",                     ui_text(UI_T_PAUSE), false },
    };
    ui_pixel_hints(s_scr, HINTS, 3);

    s_seg = 0;
    s_auto = false;
    s_paused = false;
    s_speed = SPEED_DEFAULT;
    show_segment();
    refresh_mode();
    s_timer = lv_timer_create(prompt_tick, PROMPT_TICK_MS, NULL);
    lv_screen_load(s_scr);

    esp_err_t err = app_blehid_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 启动失败 %s(联动模式无翻页,本地翻段不受影响)",
                 esp_err_to_name(err));
    }
}

void demo_prompter_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    app_blehid_shutdown();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_progress = s_mode_lbl = s_view = s_body = NULL;
    }
    if (s_text) { free(s_text); s_text = NULL; }
    if (s_line) { free(s_line); s_line = NULL; }
    s_count = 0;
    s_seg = 0;
}

void demo_prompter_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_count == 0) return;                       // 无讲稿页:只响应全局返回
    if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
        if (s_auto) { s_paused = !s_paused; refresh_mode(); }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;                // OK 长按已被 main.c 拦截回菜单
    if (btn == BSP_BTN_UP) {
        if (!s_auto) {
            s_seg = (s_seg + s_count - 1) % s_count;
            show_segment();
            app_blehid_send(-1);
        } else if (s_speed > 1) {
            s_speed--;
        }
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        if (!s_auto) {
            s_seg = (s_seg + 1) % s_count;
            show_segment();
            app_blehid_send(+1);
        } else if (s_speed < SPEED_MAX) {
            s_speed++;
        }
        return;
    }
    if (btn == BSP_BTN_OK) {                        // 切换联动/自动
        s_auto = !s_auto;
        s_paused = false;
        s_offset = 0;
        lv_obj_scroll_to_y(s_view, 0, LV_ANIM_OFF);
        refresh_mode();
    }
}
