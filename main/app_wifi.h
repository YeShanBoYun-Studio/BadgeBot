// main/app_wifi.h —— 工牌的 Wi-Fi 管理:省电脉冲 STA + 配网 SoftAP。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 启动后台任务:默认脉冲模式,用设备 NVS 里已存的凭据连接(官方固件配过网就有),
// 连上保持 CONNECT_HOLD_MS 供 SNTP 校时与数据拉取,然后断开,每小时重连一次。
void app_wifi_start(void);

// 当前 STA 是否处于已连接状态(顶部状态栏用)。
bool app_wifi_connected(void);

// ---- 翻页器互斥 ----
// 挂起 Wi-Fi:脉冲循环提前打断,驱动 stop + deinit,把堆内存让给蓝牙主机。
// 阻塞至任务完成释放(通常 ~1 秒);配网进行中返回 ESP_ERR_INVALID_STATE。
// 需与 app_wifi_resume() 配对;挂起期间 app_wifi_connected() 为 false。
esp_err_t app_wifi_suspend(void);
void app_wifi_resume(void);

// ---- 语音等在线功能的按需联网 ----
// 请求保持 STA 在线:脉冲任务立刻联网并一直保持,直到 app_wifi_hold_close()。
// 供语音上传这类"来一单用一阵"的功能;配网进行中返回 ESP_ERR_INVALID_STATE。非阻塞。
// 由同一个功能事务里配对调用 close(连接的建立与释放都归它管)。
esp_err_t app_wifi_hold_open(void);
void app_wifi_hold_close(void);

// ---- 配网模式(SoftAP + HTTP 门户) ----
// 请求进入配网模式:脉冲任务会在安全点把 Wi-Fi 切到 AP(BadgeBot-XXXX,随机密码),
// 并启动 HTTP 门户;超时或 app_wifi_portal_close() 后自动切回脉冲。非阻塞。
void app_wifi_portal_open(uint32_t timeout_ms);

// 提前结束配网模式(例如保存成功)。
void app_wifi_portal_close(void);

// 配网模式是否处于活动状态(AP 已起、门户可访问)。
bool app_wifi_portal_active(void);

// 取当前配网热点的 SSID 与密码(仅在活动期间有效;页面用它画二维码)。
bool app_wifi_portal_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// 配网剩余毫秒数(页面倒计时用)。
uint32_t app_wifi_portal_remaining_ms(void);

// 页面点「完成并联网」后调用:AP+STA 并存,STA 立刻连路由器校时;
// 热点保持开放到配网窗口结束,期间手机可继续连接与上传。
void app_wifi_portal_sync_now(void);

// 由门户在保存 Wi-Fi 后调用:延长配网窗口,让用户继续上传图片/保存资料,直到超时或"完成并联网"。
void app_wifi_portal_extend(uint32_t ms);
