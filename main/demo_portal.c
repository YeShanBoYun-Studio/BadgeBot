// main/demo_portal.c —— 配网页:起 SoftAP 后展示热点二维码(WIFI: URI)、
// 密码与剩余时间;保存 Wi-Fi 成功或超时后提示按键返回。
// 门户的 HTTP 服务由 app_wifi 任务拉起,本页只负责 UI 与启停请求。
//
// 进配网前会重启一次(RTC 慢存打标记):门户对堆完整性极度敏感
// (页面发送、TCP 缓冲、WiFi 驱动共用 ~26KB),而长开机后堆会被
// 各功能切碎(实测门户空闲堆随开关循环衰减、关联后更甚)。重启换来
// 每次配网都在干净的堆上进行;用户只多看一次开机画面。
#include "demo.h"
#include "app_wifi.h"
#include "ui_pixel.h"
#include "ui_text.h"
#include "fonts/fonts.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

#define POLL_MS 250
#define QR_SIZE 160    // 与主页二维码一致;LVGL 池 48KB 可容纳单个 160px 画布

// RTC 慢存标记:0xA5 = 本次开机是为了配网(重启前写入,上电不清零)
static RTC_NOINIT_ATTR uint8_t s_reboot_flag;

static bool s_from_reboot;    // 本次开机是"配网重启"而来(由 reboot_check 置位)

static const char *TAG_PORTAL = "demo_portal";

// 由 main.c 开机时调用:若上次重启是为了配网,消费标记并返回 true。
bool demo_portal_reboot_check(void)
{
    if (s_reboot_flag != 0xA5) return false;
    s_reboot_flag = 0;
    s_from_reboot = true;
    return true;
}

// 请求"重启进配网"(串口自动化命令 P 也走这里):只打标记,重启由调用方执行。
bool demo_portal_reboot_request(void)
{
    if (s_reboot_flag == 0xA5) return false;   // 已在流程中
    s_reboot_flag = 0xA5;
    return true;
}

static lv_obj_t *s_scr;
static lv_obj_t *s_status, *s_info, *s_url, *s_count;
static lv_timer_t *s_timer;
static bool s_qr_shown;      // 热点信息已上屏
static bool s_done;          // 配网窗口已结束

static void tick(lv_timer_t *t) {
    (void)t;
    char ssid[32], pass[16];

    if (!app_wifi_portal_active()) {
        if (s_qr_shown) {    // 从活动转为结束:停表,等用户按键返回
            s_done = true;
            lv_timer_pause(s_timer);
            lv_label_set_text(s_status, ui_text(UI_T_PORTAL_DONE));
            lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;              // 尚未启动完成(脉冲任务在收尾上一个状态),继续等
    }

    if (!s_qr_shown) {
        if (!app_wifi_portal_get(ssid, sizeof(ssid), pass, sizeof(pass))) return;
        s_qr_shown = true;

        char uri[80];
        snprintf(uri, sizeof(uri), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
        lv_obj_t *qr = lv_qrcode_create(s_scr);
        lv_qrcode_set_size(qr, QR_SIZE);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
        lv_obj_set_pos(qr, (240 - QR_SIZE) / 2, 50);
        lv_qrcode_update(qr, uri, strlen(uri));

        s_info = ui_pixel_label(s_scr, "", &font_cjk_16, ui_pixel_theme()->ink);
        lv_obj_set_pos(s_info, 0, 226);
        lv_obj_set_width(s_info, 240);
        lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(s_info, "%s  %s", ssid, pass);

        s_url = ui_pixel_label(s_scr, "", &font_cjk_16, ui_pixel_theme()->muted);
        lv_label_set_text_fmt(s_url, ui_text(UI_T_PORTAL_URL), "192.168.4.1");
        lv_obj_set_pos(s_url, 0, 254);
        lv_obj_set_width(s_url, 240);
        lv_obj_set_style_text_align(s_url, LV_TEXT_ALIGN_CENTER, 0);

        s_count = ui_pixel_label(s_scr, "", &lv_font_montserrat_16, ui_pixel_theme()->muted);
        lv_obj_set_pos(s_count, 0, 12);
        lv_obj_set_width(s_count, 158);
        lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text_fmt(s_count, "%lu s",
                          (unsigned long)(app_wifi_portal_remaining_ms() / 1000));
}

void demo_portal_enter(void) {
    // 本次开机不是为配网而来的:打标记并重启,下次开机直接落在本页
    static bool s_rebooting;
    if (!s_from_reboot && !s_rebooting) {
        s_rebooting = true;
        s_reboot_flag = 0xA5;
        ESP_LOGI(TAG_PORTAL, "配网前重启(换取干净堆)");
        esp_restart();
    }
    s_from_reboot = false;
    const ui_theme_t *th = ui_pixel_theme();
    s_scr = ui_pixel_screen_create("PORTAL");
    s_qr_shown = false;
    s_done = false;
    s_status = ui_pixel_label(s_scr, ui_text(UI_T_PORTAL_STARTING), &font_cjk_16, th->muted);
    lv_obj_set_pos(s_status, 0, 150);
    lv_obj_set_width(s_status, 240);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    const ui_hint_t PORTAL_HINTS[] = {
        { "OK", ui_text(UI_T_EXIT), false },
    };
    ui_pixel_hints(s_scr, PORTAL_HINTS, 1);

    app_wifi_portal_open(0);
    s_timer = lv_timer_create(tick, POLL_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_portal_exit(void) {
    app_wifi_portal_close();                 // 用户提前离开也要收掉热点
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = s_info = s_url = s_count = NULL;
    }
}

void demo_portal_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    (void)btn;
    if (s_done && ev == BSP_BTN_CLICK) {
        demo_request_menu();
    }
}
