// main/demo_voice.c —— V2 语音页:OK 开始说话,再按 OK 停止并发送;
// 识别文本回显在屏上,同时由电脑后端注入键盘。长按 OK 返回菜单(main.c 拦截)。
#include "demo.h"
#include "app_config.h"
#include "app_voice.h"
#include "app_voice_model.h"
#include "fonts/fonts.h"
#include "ui_pixel.h"
#include "ui_text.h"

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr, *s_status, *s_detail;
static lv_timer_t *s_timer;
static char s_result[VOICE_TEXT_CAP];
static bool s_has_result;

static const char *err_text(voice_err_t e)
{
    switch (e) {
    case VOICE_ERR_NOURL: return ui_text(UI_T_VOICE_NOURL);
    case VOICE_ERR_NET:   return ui_text(UI_T_VOICE_E_NET);
    case VOICE_ERR_CONN:  return ui_text(UI_T_VOICE_E_CONN);
    case VOICE_ERR_SEND:  return ui_text(UI_T_VOICE_E_SEND);
    case VOICE_ERR_HTTP:  return ui_text(UI_T_VOICE_E_HTTP);
    case VOICE_ERR_EMPTY: return ui_text(UI_T_VOICE_E_EMPTY);
    default:              return ui_text(UI_T_VOICE_E_MIC);
    }
}

// LVGL 定时器上下文里直接操作控件(同任务,无需加锁)
static void render(void)
{
    char buf[APP_CFG_URL_LEN + 48];
    switch (app_voice_state()) {
    case VOICE_WAIT_NET:
        lv_label_set_text(s_status, ui_text(UI_T_VOICE_NET));
        lv_label_set_text(s_detail, "");
        break;
    case VOICE_REC: {
        uint32_t ms = app_voice_elapsed_ms();
        snprintf(buf, sizeof(buf), "%s  %lu:%02lu", ui_text(UI_T_VOICE_REC),
                 (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60));
        lv_label_set_text(s_status, buf);
        lv_label_set_text(s_detail, ui_text(UI_T_VOICE_TAP_STOP));
        break;
    }
    case VOICE_SEND:
        lv_label_set_text(s_status, ui_text(UI_T_VOICE_SENDING));
        lv_label_set_text(s_detail, "");
        break;
    case VOICE_DONE:
        lv_label_set_text(s_status, ui_text(UI_T_VOICE_DONE));
        lv_label_set_text(s_detail, s_has_result ? s_result : "");
        break;
    case VOICE_ERR:
        lv_label_set_text(s_status, err_text(app_voice_error()));
        lv_label_set_text(s_detail, "");
        break;
    default: {   // IDLE:显示后端地址(或提示先配置)与操作提示
        const app_config_t *cfg = app_config_get();
        char host[APP_CFG_URL_LEN];
        voice_host_display(cfg->voice_url, host, sizeof(host));
        // 文案表条目不作 printf 格式串使用(见 ui_text.h 说明)
        snprintf(buf, sizeof(buf), "%s\n%s", host, ui_text(UI_T_VOICE_READY));
        lv_label_set_text(s_status, ui_text(UI_T_VOICE_ACT));
        if (voice_url_valid(cfg->voice_url)) {
            lv_label_set_text(s_detail, buf);
        } else {
            lv_label_set_text(s_detail, ui_text(UI_T_VOICE_NOURL));
        }
        break;
    }
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    voice_state_t st = app_voice_state();
    if (st == VOICE_DONE && !s_has_result) {
        char tmp[VOICE_TEXT_CAP];
        if (app_voice_take_text(tmp, sizeof(tmp))) {
            snprintf(s_result, sizeof(s_result), "%s", tmp);
            s_has_result = true;
        }
    }
    if (st != VOICE_DONE) s_has_result = false;
    render();
}

void demo_voice_enter(void)
{
    s_scr = ui_pixel_screen_create("VOICE");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 11, 52, 218, 200, UI_PAPER);

    s_status = ui_pixel_label(panel, "", &font_cjk_20, ui_pixel_theme()->ink);
    lv_obj_set_pos(s_status, 0, 16);
    lv_obj_set_width(s_status, 196);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    s_detail = ui_pixel_label(panel, "", &font_cjk_16, ui_pixel_theme()->muted);
    lv_obj_set_pos(s_detail, 0, 56);
    lv_obj_set_width(s_detail, 196);
    lv_obj_set_height(s_detail, 120);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);

    const ui_hint_t HINTS[] = {
        { "OK", ui_text(UI_T_VOICE_ACT), false },
        { "OK", ui_text(UI_T_MENU),      true },
    };
    ui_pixel_hints(s_scr, HINTS, 2);

    s_result[0] = '\0';
    s_has_result = false;
    render();
    s_timer = lv_timer_create(tick, 150, NULL);
    lv_screen_load(s_scr);
}

void demo_voice_exit(void)
{
    // 录音中离开:提前收尾送出(worker 与页面解耦,自己会结束)
    app_voice_stop();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = s_status = s_detail = NULL;
    }
}

void demo_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn != BSP_BTN_OK || ev != BSP_BTN_CLICK) return;
    voice_state_t st = app_voice_state();
    if (st == VOICE_REC || st == VOICE_WAIT_NET) app_voice_stop();
    else if (st == VOICE_IDLE || st == VOICE_DONE || st == VOICE_ERR) app_voice_start();
}
