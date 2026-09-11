// main/demo.h —— 每个演示页实现的统一接口。
// 新增一个演示页 = 实现这三个函数 + 在 main.c 的 DEMOS[] 里加一行。
#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;        // 英文名(设置 lang=1 或未译时显示)
    const char *name_zh;     // 中文名(lang=0 时显示;可为 NULL 回退英文)
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键(长按确定已被 main 拦截)
} demo_entry_t;

// 由 main.c 实现:页面在自己的 key 回调里请求返回菜单(等价于长按确定),
// 供主页这类"短按确定 = 打开菜单"的页面使用。只能在 key 回调内调用。
void demo_request_menu(void);
// 由 main.c 实现:立即关背光进入息屏,任意键唤醒(唤醒键不分发给页面)。
// 自动息屏由 main.c 按设置的分钟数统一处理,页面无需自己计时。
void demo_request_screen_off(void);

// 各演示页(定义在各自的 .c 里)
void demo_badge_enter(void);   void demo_badge_exit(void);
void demo_badge_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_settings_enter(void); void demo_settings_exit(void);
void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_portal_enter(void);  void demo_portal_exit(void);
void demo_portal_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_display_enter(void); void demo_display_exit(void);
void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_button_enter(void);  void demo_button_exit(void);
void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_audio_enter(void);   void demo_audio_exit(void);
void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_battery_enter(void); void demo_battery_exit(void);
void demo_battery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_ble_enter(void);     void demo_ble_exit(void);
void demo_ble_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_hid_enter(void);     void demo_hid_exit(void);
void demo_hid_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_prompter_enter(void); void demo_prompter_exit(void);
void demo_prompter_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_voice_enter(void);   void demo_voice_exit(void);
void demo_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_low_power_enter(void); void demo_low_power_exit(void);
void demo_low_power_key(bsp_btn_t btn, bsp_btn_ev_t ev);
