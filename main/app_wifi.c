// main/app_wifi.c —— Wi-Fi 管理:省电脉冲 STA + 配网 SoftAP,单一属主任务。
//
// 为什么是脉冲:电池只有 520mAh,STA 常连平均 ~100mA 只能撑几小时。
// 每小时连一次、保持一分钟,足够 SNTP 校时和以后 GitHub 数据的定期拉取。
//
// 配网模式:收到 portal 请求后,任务在安全点断开 STA、切到 AP(BadgeBot-XXXX,
// 随机 8 位密码),拉起 HTTP 门户(app_portal),超时/保存/手动关闭后切回 STA。
// 整个过程只有一个任务碰 esp_wifi,不存在并发初始化。
//
// 凭据来源:STA 用 WIFI_STORAGE_FLASH —— 门户里保存的 Wi-Fi 会写进 NVS,
// 之后每次脉冲零配置自动连接。
#include "app_wifi.h"
#include "app_clock.h"
#include "app_portal.h"
#include "demo_radio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define CONNECT_TIMEOUT_MS 20000
#define CONNECT_HOLD_MS    60000     // 连上后保持时长:足够 SNTP 与后续拉取
#define RETRY_INTERVAL_MS  300000    // 未连上时的重试间隔
#define RESYNC_INTERVAL_MS 3600000   // 正常重连间隔(每小时校时/拉数据)
#define PORTAL_TIMEOUT_MS  180000    // 配网门户默认时长
#define PORTAL_SAVED_MS    3000      // 保存成功后停留片刻再关闭,让手机看到回执

static const char *TAG = "app_wifi";

static volatile bool s_connected;
static TaskHandle_t s_task;
static EventGroupHandle_t s_events;
#define EV_PORTAL BIT0
#define EV_EXIT   BIT1

// 配网模式状态(任务写,页面/HTTP 读;读取处不要求强一致)
static volatile bool s_portal_active;
static volatile bool s_portal_saved;
static uint32_t s_portal_deadline;
static uint32_t s_portal_saved_at;
static char s_ap_ssid[32], s_ap_pass[16];

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

// ---- 脉冲 STA ----

static void pulse_connect_cycle(void)
{
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
    } else {
        ESP_LOGW(TAG, "连接失败(没有已存凭据或信号不佳),%d 分钟后重试",
                 RETRY_INTERVAL_MS / 60000);
    }
    esp_wifi_disconnect();
}

// ---- 配网 SoftAP ----

static void gen_random_pass(char *out, size_t len)
{
    // 8 位大写字母+数字(去掉易混淆的 0/O/1/I)
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    for (size_t i = 0; i + 1 < len && i < sizeof(rnd); i++) {
        out[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
    }
    out[len - 1] = '\0';
}

static void portal_setup_ap(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "BadgeBot-%02X%02X", mac[4], mac[5]);
    gen_random_pass(s_ap_pass, sizeof(s_ap_pass));

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 2;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    ESP_LOGI(TAG, "配网热点: %s / %s(日志打印仅用于开发,量产版移除)", s_ap_ssid, s_ap_pass);
}

static void run_portal(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "进入配网模式");
    s_portal_saved = false;
    s_portal_deadline = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + timeout_ms;

    esp_wifi_disconnect();
    esp_wifi_stop();
    portal_setup_ap();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();
    if (app_portal_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 门户启动失败,退出配网模式");
    } else {
        s_portal_active = true;
        ESP_LOGI(TAG, "配网门户就绪: http://192.168.4.1");

        // 门户窗口:超时、保存成功(延迟片刻)或页面上提前关闭,任一即退出
        for (;;) {
            EventBits_t bits = xEventGroupWaitBits(s_events, EV_EXIT, pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(250));
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((bits & EV_EXIT) ||
                (int32_t)(now - s_portal_deadline) >= 0 ||
                (s_portal_saved && (int32_t)(now - s_portal_saved_at) >= PORTAL_SAVED_MS)) {
                break;
            }
        }
        s_portal_active = false;
        app_portal_http_stop();
        ESP_LOGI(TAG, "退出配网模式");
    }

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

// ---- 任务主体 ----

static void wifi_task(void *arg)
{
    (void)arg;
    esp_err_t e = demo_radio_nvs_prepare();
    if (e != ESP_OK) { ESP_LOGE(TAG, "NVS 不可用,Wi-Fi 任务退出"); vTaskDelete(NULL); return; }
    e = demo_radio_network_prepare();
    if (e != ESP_OK) { ESP_LOGE(TAG, "netif/事件循环初始化失败: %s", esp_err_to_name(e)); vTaskDelete(NULL); return; }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "默认 STA netif 创建失败,Wi-Fi 任务退出");
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
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);   // STA 凭据持久化,门户保存后每次脉冲自动连
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    s_events = xEventGroupCreate();

    for (;;) {
        // 等待下一次脉冲(期间收到配网请求则立即切换)
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_PORTAL | EV_EXIT, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(RESYNC_INTERVAL_MS));
        if (bits & EV_PORTAL) {
            run_portal(PORTAL_TIMEOUT_MS);
            pulse_connect_cycle();   // 配网后立刻用新凭据联网校时,不让用户等一小时
            continue;
        }
        if (bits & EV_EXIT) break;

        pulse_connect_cycle();
    }
    vTaskDelete(NULL);
}

void app_wifi_start(void)
{
    if (xTaskCreate(wifi_task, "app_wifi", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi 任务创建失败");
    }
}

bool app_wifi_connected(void)
{
    return s_connected;
}

// ---- 配网模式 API ----

void app_wifi_portal_open(uint32_t timeout_ms)
{
    if (s_events) xEventGroupSetBits(s_events, EV_PORTAL);
    (void)timeout_ms;   // 时长固定为 PORTAL_TIMEOUT_MS,由任务侧决定
}

void app_wifi_portal_close(void)
{
    if (s_events) xEventGroupSetBits(s_events, EV_EXIT);
}

bool app_wifi_portal_active(void)
{
    return s_portal_active;
}

bool app_wifi_portal_get(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (!s_portal_active) return false;
    if (ssid) strlcpy(ssid, s_ap_ssid, ssid_len);
    if (pass) strlcpy(pass, s_ap_pass, pass_len);
    return true;
}

uint32_t app_wifi_portal_remaining_ms(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int32_t left = (int32_t)(s_portal_deadline - now);
    return left > 0 ? (uint32_t)left : 0;
}

void app_portal_notify_saved(void)
{
    s_portal_saved = true;
    s_portal_saved_at = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
