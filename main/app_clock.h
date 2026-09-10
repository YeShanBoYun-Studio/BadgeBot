// main/app_clock.h —— SNTP 校时与本地时间。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// 设时区(中国标准时间 UTC+8)。SNTP 不在此启动:没有网络时启动只是空转。
void app_clock_init(void);

// 由 app_wifi 在拿到 IP 后调用:启动 SNTP 并向 ntp.aliyun.com / pool.ntp.org 校时。
// 之后 SNTP 周期性重校(约每小时),只要 app_wifi 的脉冲保持期覆盖到即可。
void app_clock_network_up(void);

// 是否已通过 SNTP 完成过至少一次校时。
bool app_clock_synced(void);

// 已校时则把本地时间填入 out 并返回 true;未校时返回 false。
bool app_clock_local(struct tm *out);
