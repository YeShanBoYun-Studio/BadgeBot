// main/app_github.h —— GitHub 提交热力图:联网窗口拉取 + NVS 缓存。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GH_DAYS       371        // 53 周
#define GH_LEVELS     5          // GitHub 热度 0..4
#define GH_CACHE_BYTES 96        // 2bit/天 × 371 天 = 93 字节,取整到 96

// 从 NVS 读缓存(开机调用一次;无缓存则为空状态)。
void app_github_load(void);

// 联网窗口内同步拉取当前配置用户的贡献热力图(阻塞数秒;失败静默保留旧缓存)。
// 数据源:github-contributions-api.jogruber.de(免鉴权);用户名为空时直接返回。
void app_github_fetch(void);

// 是否有可显示的缓存数据。
bool app_github_ready(void);

// 第 day 天(0 = 最早一天)的热度级别 0..4。
int app_github_level(int day);

// 缓存覆盖周期内的提交总数。
int app_github_total(void);
