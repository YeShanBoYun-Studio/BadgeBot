// main/app_store.h —— 头像/二维码图片等上传资产的 FATFS 存储。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 挂载 store 分区(FATFS,256KB)到 /store。幂等;分区不存在或挂载失败不致命,
// 依赖文件的功能(头像上传等)各自降级。新分区首次挂载前会自动格式化。
esp_err_t app_store_init(void);

// /store 是否可用。
bool app_store_ready(void);

// 写入/覆盖一个资产文件(数据长度为 0 时删除该文件)。
esp_err_t app_store_write(const char *name, const void *data, size_t len);

// 读取资产文件到 buf(不超过 buf_len),返回实际字节数;-1 = 不存在或失败。
int app_store_read(const char *name, void *buf, size_t buf_len);

// 查询资产文件字节数;-1 = 不存在或失败(按需精确分配读缓冲用)。
long app_store_size(const char *name);
