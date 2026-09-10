// main/app_portal.h —— 配网门户:SoftAP 模式下的 HTTP 配置页。
#pragma once

#include "esp_err.h"

// 启动/停止 HTTP 服务(仅配网窗口内由 app_wifi 调用)。
esp_err_t app_portal_http_start(void);
void app_portal_http_stop(void);
