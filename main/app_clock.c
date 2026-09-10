// main/app_clock.c —— SNTP 校时与时区。
// 时区固定为中国标准时间(CST-8);服务器优先阿里云 NTP,公网池兜底。
#include "app_clock.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "app_clock";
static volatile bool s_synced;

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP 校时完成");
}

void app_clock_init(void)
{
    setenv("TZ", "CST-8", 1);   // 中国标准时间,无夏令时
    tzset();
}

void app_clock_network_up(void)
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 启动(网络已就绪)");
}

bool app_clock_synced(void)
{
    return s_synced;
}

bool app_clock_local(struct tm *out)
{
    if (!s_synced) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}
