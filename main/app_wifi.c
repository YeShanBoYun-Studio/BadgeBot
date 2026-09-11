// main/app_wifi.c —— Wi-Fi 管理:省电脉冲 STA + 配网 SoftAP,单一属主任务。
//
// 为什么是脉冲:电池只有 520mAh,STA 常连平均 ~100mA 只能撑几小时。
// 每小时连一次、保持一分钟,足够 SNTP 校时。
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
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "dns_server.h"
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
#define EV_PORTAL  BIT0
#define EV_EXIT    BIT1
#define EV_SUSPEND BIT2   // BLE 翻页器要独占无线电:脉冲任务停驱动并释放内存
#define EV_RESUME  BIT3   // 翻页器退出:脉冲任务重新初始化驱动
#define EV_HOLD    BIT4   // 语音等在线功能请求联网并保持(hold_serve_cycle 承接)

// 翻页器挂起:请求置位给 pulse 循环提前打断;suspend_sem 用于等任务完成 deinit
static volatile bool s_suspend_req;
static SemaphoreHandle_t s_suspend_sem;

// 按需联网请求:置位期间 STA 保持连接;由语音 worker 在事务结束时清除
static volatile bool s_hold_req;

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
    while (!s_connected && waited < CONNECT_TIMEOUT_MS && !s_suspend_req) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (s_connected) {
        ESP_LOGI(TAG, "已连接,保持 %d 秒(校时窗口)", CONNECT_HOLD_MS / 1000);
        int held = 0;
        // hold 请求让常规保持窗口立即让位:EV_HOLD 的承接循环马上接管连接
        while (held < CONNECT_HOLD_MS && !s_suspend_req && !s_hold_req) {
            vTaskDelay(pdMS_TO_TICKS(250));
            held += 250;
        }
    } else {
        ESP_LOGW(TAG, "连接失败(没有已存凭据或信号不佳),%d 分钟后重试",
                 RETRY_INTERVAL_MS / 60000);
    }
    esp_wifi_disconnect();
}

// ---- 按需联网(语音) ----

// 承接 hold 请求:确保连上,然后一直保持到请求方关闭;结束时断开归位脉冲节奏。
static void hold_serve_cycle(void)
{
    if (!s_connected) {
        ESP_LOGI(TAG, "按需联网请求(语音等在线功能)");
        esp_wifi_connect();
        int waited = 0;
        while (!s_connected && waited < CONNECT_TIMEOUT_MS && !s_suspend_req && s_hold_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }
    if (s_connected) {
        while (s_connected && s_hold_req && !s_suspend_req) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        esp_wifi_disconnect();
    }
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
    // 热点名每次配网随机化(用户要求不固定):BadgeBot-XXXX,密码同为随机
    char suffix[5];
    gen_random_pass(suffix, sizeof(suffix));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "BadgeBot-%s", suffix);
    gen_random_pass(s_ap_pass, sizeof(s_ap_pass));

    // 注意顺序:必须先切到 AP 模式再写 AP 配置,否则 set_config 返回
    // ESP_ERR_WIFI_MODE 被静默忽略,热点会以空配置广播(手机看不到 BadgeBot)。
    // 注意:门户期间必须用 APSTA(而不是纯 AP)——STA 接口存在,门户保存的
    // Wi-Fi 凭据才能通过 esp_wifi_set_config(WIFI_IF_STA) 写入;纯 AP 下会报
    // ESP_ERR_WIFI_MODE。模式切换须先于配置写入。
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "切 AP 模式失败: %s", esp_err_to_name(e));
        return;
    }

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 2;
    if ((e = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "AP 配置写入失败: %s", esp_err_to_name(e));
        return;
    }
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
    esp_wifi_start();
    esp_wifi_disconnect();   // 配网期间别让 STA 拿旧凭据自动连接
    // 强制门户:捕获所有 DNS 查询,手机连上热点会自动弹出配置页(官方 captive_portal 做法)
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server_handle_t dns_handle = start_dns_server(&dns_cfg);
    if (dns_handle == NULL) {
        ESP_LOGW(TAG, "DNS 服务器启动失败,只能手动访问 192.168.4.1");
    }
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
        if (dns_handle) stop_dns_server(dns_handle);
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
    // AP netif 自带 DHCP 服务器:没有它手机连上热点拿不到 IP,门户不可达(官方 softAP
    // 示例同样在 esp_wifi_init 之后创建 AP netif)。
    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "默认 AP netif 创建失败,Wi-Fi 任务退出");
        vTaskDelete(NULL);
        return;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // C3 无 PSRAM,默认 10 静态 RX×1600B 偏重(徽章流量小);缩到 6/16 降低 NO_MEM 概率
    cfg.static_rx_buf_num = 6;
    cfg.dynamic_rx_buf_num = 16;
    // 开机瞬间的堆竞争可能让 esp_wifi_init 拿不到内存:延迟重试而不是永久放弃联网
    for (int attempt = 1;; attempt++) {
        e = esp_wifi_init(&cfg);
        if (e == ESP_OK) break;
        ESP_LOGE(TAG, "esp_wifi_init 失败(%s),第 %d 次重试等 10s", esp_err_to_name(e), attempt);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);   // STA 凭据持久化,门户保存后每次脉冲自动连
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    s_events = xEventGroupCreate();

    pulse_connect_cycle();   // 开机先脉冲一次,尽快校时,不让用户等满一小时
    for (;;) {
        // 等待下一次脉冲(期间收到配网/挂起/按需联网请求则立即切换)
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_PORTAL | EV_EXIT | EV_SUSPEND | EV_HOLD,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(RESYNC_INTERVAL_MS));
        if (bits & EV_PORTAL) {
            run_portal(PORTAL_TIMEOUT_MS);
            pulse_connect_cycle();   // 配网后立刻用新凭据联网校时,不让用户等一小时
            continue;
        }
        if (bits & EV_EXIT) break;
        if (bits & EV_SUSPEND) {
            // BLE 翻页器独占无线电:彻底释放驱动内存(蓝牙主机约需 40KB 堆),
            // 等翻页器退出后原地重新初始化,继续原有脉冲节奏。
            esp_wifi_disconnect();
            esp_wifi_stop();
            esp_wifi_deinit();
            xSemaphoreGive(s_suspend_sem);
            xEventGroupWaitBits(s_events, EV_RESUME, pdTRUE, pdFALSE, portMAX_DELAY);
            for (int attempt = 1;; attempt++) {
                e = esp_wifi_init(&cfg);
                if (e == ESP_OK) break;
                ESP_LOGE(TAG, "esp_wifi_init 失败(%s),第 %d 次重试等 10s",
                         esp_err_to_name(e), attempt);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_start();
            continue;
        }
        if (bits & EV_HOLD) {
            hold_serve_cycle();
            continue;
        }

        pulse_connect_cycle();
    }
    vTaskDelete(NULL);
}

void app_wifi_start(void)
{
    // 12KB 栈:esp_http_client + TLS 握手(X509 验签)在本任务内同步执行,
    // 6KB 时握手期的深调用曾把验签数据踩坏(证书报"签名验证失败 0x4290")
    if (xTaskCreate(wifi_task, "app_wifi", 12288, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi 任务创建失败");
    }
}

bool app_wifi_connected(void)
{
    return s_connected;
}

// ---- 翻页器互斥:挂起/恢复 ----

esp_err_t app_wifi_suspend(void)
{
    if (s_portal_active) return ESP_ERR_INVALID_STATE;   // 配网期间不外借无线电
    if (s_hold_req) return ESP_ERR_INVALID_STATE;        // 在线功能持有连接期间同理
    if (s_suspend_req) return ESP_OK;                    // 已在挂起流程中
    // 任务还没跑到建 s_events(开机头一两秒):稍等它就绪
    for (int i = 0; !s_events && i < 50; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_events) return ESP_ERR_INVALID_STATE;

    s_suspend_req = true;
    s_suspend_sem = xSemaphoreCreateBinary();
    if (!s_suspend_sem) return ESP_ERR_NO_MEM;
    xEventGroupSetBits(s_events, EV_SUSPEND);
    // 正常在 1 秒内完成(pulse 循环会因 s_suspend_req 提前退出);超时兜底返回
    if (xSemaphoreTake(s_suspend_sem, pdMS_TO_TICKS(12000)) != pdTRUE) {
        s_suspend_req = false;
        vSemaphoreDelete(s_suspend_sem);
        s_suspend_sem = NULL;
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(s_suspend_sem);
    s_suspend_sem = NULL;
    ESP_LOGI(TAG, "Wi-Fi 已挂起,无线电让给蓝牙");
    return ESP_OK;
}

void app_wifi_resume(void)
{
    if (!s_suspend_req) return;
    s_suspend_req = false;
    if (s_events) xEventGroupSetBits(s_events, EV_RESUME);
    ESP_LOGI(TAG, "Wi-Fi 恢复脉冲");
}

// ---- 按需联网:语音等在线功能 ----

esp_err_t app_wifi_hold_open(void)
{
    if (s_portal_active) return ESP_ERR_INVALID_STATE;   // 配网期间 STA 不可用
    // 任务还没跑到建 s_events(开机头一两秒):稍等它就绪
    for (int i = 0; !s_events && i < 50; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_events) return ESP_ERR_INVALID_STATE;

    if (s_hold_req) return ESP_OK;                       // 已在保持中
    s_hold_req = true;
    xEventGroupSetBits(s_events, EV_HOLD);
    return ESP_OK;
}

void app_wifi_hold_close(void)
{
    s_hold_req = false;
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

void app_wifi_portal_extend(uint32_t ms)
{
    // 保存 Wi-Fi 后热点不立刻关:给用户留出继续上传图片/保存资料的时间
    s_portal_deadline += ms;
}

void app_portal_notify_saved(void)
{
    s_portal_saved = true;
    s_portal_saved_at = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
