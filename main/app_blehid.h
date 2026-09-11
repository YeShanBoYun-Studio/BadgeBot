// main/app_blehid.h —— BLE HID 翻页器:工牌以蓝牙键盘身份出现,UP/DOWN 发翻页键。
// HID-over-GATT 服务按规范自实现(IDF 5.5 无 HID 设备例程);配对用 Just Works,
// 绑定密钥经 NimBLE NVS 持久化(回摘 CONFIG_BT_NIMBLE_NVS_PERSIST)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"

// 启动 NimBLE + HID 服务并开始广播;幂等。失败返回错误码,状态回 IDLE。
esp_err_t app_blehid_start(void);
// 停止广播、断开连接并停掉 NimBLE 主机(退页调用;幂等)。
void app_blehid_shutdown(void);

// 发一次翻页键:dir = -1 上一页 / +1 下一页(键位映射读 app_config.hid_arrows)。
// 只有"已加密连接"时才真正发送;未连接时静默丢弃。
void app_blehid_send(int dir);

typedef enum {
    BLEHID_IDLE = 0,      // 未启动或启动失败
    BLEHID_ADVERTISING,   // 广播中,等待电脑/手机配对
    BLEHID_CONNECTED,     // 已连接但尚未加密(配对流程中)
    BLEHID_READY,         // 已加密,按键可发送
} blehid_state_t;

blehid_state_t app_blehid_state(void);

#endif
