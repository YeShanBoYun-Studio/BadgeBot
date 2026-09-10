// main/app_store.h —— 头像/二维码图片等上传资产的 FATFS 存储。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 挂载 store 分区(FATFS,256KB)到 /store。幂等;分区不存在或挂载失败不致命,
// 依赖文件的功能(头像上传等)各自降级。新分区首次挂载前会自动格式化。
esp_err_t app_store_init(void);

// /store 是否可用。
bool app_store_ready(void);
