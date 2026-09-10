// main/app_wifi.h —— 工牌的 Wi-Fi 管理:省电脉冲 STA + 配网 SoftAP。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 启动后台任务:默认脉冲模式,用设备 NVS 里已存的凭据连接(官方固件配过网就有),
// 连上保持 CONNECT_HOLD_MS 供 SNTP 校时与数据拉取,然后断开,每小时重连一次。
void app_wifi_start(void);

// 当前 STA 是否处于已连接状态(顶部状态栏用)。
bool app_wifi_connected(void);

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

// 由门户在 Wi-Fi 凭据保存成功后调用:配网窗口将停留数秒并自动关闭。
void app_portal_notify_saved(void);
