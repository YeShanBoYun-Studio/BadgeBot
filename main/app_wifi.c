// main/app_wifi.c —— Wi-Fi 脉冲连接任务。
//
// 为什么是脉冲:电池只有 520mAh,STA 常连平均 ~100mA 只能撑几小时。
// 每小时连一次、保持一分钟,足够 SNTP 校时和以后 GitHub 数据的定期拉取;
// 期间 app_clock_network_up() 会启动 SNTP,断网后 SNTP 的重试静默失败,无害。
//
// 凭据来源:esp_wifi_set_storage(WIFI_STORAGE_FLASH) 直接使用 NVS 分区里
// 已保存的 STA 配置 —— 设备此前用官方固件配过网的话,这里零配置即可上网。
#include "app_wifi.h"
#include "app_clock.h"
#include "demo_radio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONNECT_TIMEOUT_MS 20000
#define CONNECT_HOLD_MS    60000     // 连上后保持时长:足够 SNTP 与后续拉取
#define RETRY_INTERVAL_MS  300000    // 未连上时的重试间隔
#define RESYNC_INTERVAL_MS 3600000   // 正常重连间隔(每小时校时/拉数据)

static const char *TAG = "app_wifi";
static volatile bool s_connected;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        app_clock_network_up();
    }
}

static void wifi_task(void *arg)
{
    (void)arg;
    esp_err_t e = demo_radio_nvs_prepare();
    if (e != ESP_OK) { ESP_LOGE(TAG, "NVS 不可用,Wi-Fi 脉冲退出"); vTaskDelete(NULL); return; }
    e = demo_radio_network_prepare();
    if (e != ESP_OK) { ESP_LOGE(TAG, "netif/事件循环初始化失败: %s", esp_err_to_name(e)); vTaskDelete(NULL); return; }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "默认 STA netif 创建失败,Wi-Fi 脉冲退出");
        vTaskDelete(NULL);
        return;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);   // 复用设备里已存的凭据
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    for (;;) {
        ESP_LOGI(TAG, "尝试连接 Wi-Fi(使用设备已存凭据)");
        esp_wifi_connect();
        int waited = 0;
        while (!s_connected && waited < CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        if (s_connected) {
            ESP_LOGI(TAG, "已连接,保持 %d 秒(校时/拉取窗口)", CONNECT_HOLD_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(CONNECT_HOLD_MS));
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(RESYNC_INTERVAL_MS));
        } else {
            ESP_LOGW(TAG, "连接失败(没有已存凭据或信号不佳),%d 分钟后重试",
                     RETRY_INTERVAL_MS / 60000);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
        }
    }
}

void app_wifi_start(void)
{
    if (xTaskCreate(wifi_task, "app_wifi", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi 脉冲任务创建失败");
    }
}

bool app_wifi_connected(void)
{
    return s_connected;
}
