// main/app_wifi.h —— 工牌的 Wi-Fi 脉冲连接(省电模式)。
#pragma once

#include <stdbool.h>

// 启动后台脉冲任务:用设备 NVS 里已存的凭据连接(官方固件配过网就有),
// 连上保持 CONNECT_HOLD_MS 供 SNTP 校时与后续数据拉取,然后断开,
// 每小时重连一次;连不上(没配过网/信号差)每 5 分钟重试。
// 本模块独占 Wi-Fi:原 Wi-Fi 扫描演示页已从菜单移除,M2 的连接页将基于本模块重建。
void app_wifi_start(void);

// 当前 STA 是否处于已连接状态(顶部状态栏用)。
bool app_wifi_connected(void);
