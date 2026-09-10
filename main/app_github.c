// main/app_github.c —— GitHub 提交热力图。
//
// 数据源:github-contributions-api.jogruber.de/v4/<user>(免费公开接口,返回
// 最近一年的逐日 level 0..4)。host 是编译期常量,用户名在配置层已过滤为
// [A-Za-z0-9-_],不存在任意 URL 注入。
//
// 拉取发生在 Wi-Fi 脉冲窗口内(已连接状态)。响应体用 2KB 滚动块流式解析,
// 全程只保留 64B 尾巴 —— 早期版本整段缓存 JSON(24KB),叠加 TLS ~40KB 后
// 在开机脉冲时把堆挤爆(mbedtls_ssl_setup 报 -0x7F00,热力图永远拉不到)。
// 失败静默保留旧缓存,页面永远有内容可画。
#include "app_github.h"
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

// 证书验证:走全局证书包。此前所有证书验签失败(-0x4290)的根因是 C3 的 RSA
// 硬件加速上限 3072 位,RSA-4096 证书回退软件路径后滑窗表(默认 ~33KB 连续内存)
// 分配失败;sdkconfig 把 MBEDTLS_MPI_WINDOW_SIZE 降到 2 后恢复。

static const char *TAG = "app_gh";
#define NVS_NS "badgebot_v2"
#define MAX_DAYS 400
#define CHUNK_LEN 2048
#define TAIL_MAX 64           // 跨块截断的 token 至多这么长
#define WORK_LEN (TAIL_MAX + CHUNK_LEN)

static uint8_t s_levels[GH_DAYS];        // 0..4,255 = 无数据
static int s_total;
static bool s_ready;

// 单任务调用(wifi 任务),解析工作区静态化,免栈尖峰
typedef struct {
    char    tail[TAIL_MAX];
    int     tail_len;
    int     pending_count;   // 已见 "count":,等相邻 "level": 配对
    bool    have_count;
    int     day;
    int     total;
    char    work[WORK_LEN + 1];
    char    chunk[CHUNK_LEN];
} parse_state_t;
static parse_state_t s_ps;

void app_github_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_levels);
    if (nvs_get_blob(h, "gh_lv", s_levels, &len) == ESP_OK && len == sizeof(s_levels)) {
        int32_t total = 0;
        nvs_get_i32(h, "gh_total", &total);
        s_total = (int)total;
        s_ready = true;
        ESP_LOGI(TAG, "热力图缓存: 共 %d 次", s_total);
    }
    nvs_close(h);
}

bool app_github_ready(void) { return s_ready; }

int app_github_level(int day)
{
    if (day < 0 || day >= GH_DAYS || !s_ready) return -1;
    return s_levels[day];
}

int app_github_total(void) { return s_total; }

static void cache_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "gh_lv", s_levels, sizeof(s_levels)) == ESP_OK)
        nvs_set_i32(h, "gh_total", s_total);
    nvs_commit(h);
    nvs_close(h);
}

static void parse_reset(void)
{
    memset(s_levels, 255, sizeof(s_levels));
    s_ps.tail_len = 0;
    s_ps.pending_count = 0;
    s_ps.have_count = false;
    s_ps.day = 0;
    s_ps.total = 0;
}

// 顺序抽取 "count":N 与相邻的 "level":N;token 被块边界截断时留在尾巴里等下一块。
// 接口返回的逐日对象形如 {"date":...,"count":N,"level":M}(count 在前)。
static void parse_feed(const char *data, int n)
{
    if (n <= 0) return;
    memcpy(s_ps.work, s_ps.tail, s_ps.tail_len);
    memcpy(s_ps.work + s_ps.tail_len, data, n);
    int len = s_ps.tail_len + n;
    s_ps.work[len] = '\0';

    int pos = 0;
    for (;;) {
        char *c = strstr(s_ps.work + pos, "\"count\":");
        char *l = strstr(s_ps.work + pos, "\"level\":");
        if (c && (!l || c < l)) {
            s_ps.pending_count = atoi(c + 8);
            s_ps.have_count = true;
            pos = (int)(c - s_ps.work) + 8;
        } else if (l) {
            int level = atoi(l + 8);
            if (level < 0) level = 0;
            if (level > 4) level = 4;
            if (s_ps.day < MAX_DAYS) {
                s_levels[s_ps.day++] = (uint8_t)level;
                s_ps.total += s_ps.have_count ? s_ps.pending_count : 0;
            }
            s_ps.have_count = false;
            pos = (int)(l - s_ps.work) + 8;
        } else {
            // 没有完整 token:留尾巴等下一块(尾巴里可能藏着被截断的 token)
            s_ps.tail_len = len - pos > TAIL_MAX ? TAIL_MAX : len - pos;
            memmove(s_ps.tail, s_ps.work + len - s_ps.tail_len, s_ps.tail_len);
            return;
        }
        if (len - pos <= TAIL_MAX) {
            s_ps.tail_len = len - pos;
            memmove(s_ps.tail, s_ps.work + pos, s_ps.tail_len);
            return;
        }
    }
}

static void parse_finish(void)
{
    if (s_ps.day > 0) {
        s_total = s_ps.total;
        s_ready = true;
        ESP_LOGI(TAG, "热力图解析: %d 天,共 %d 次", s_ps.day, s_ps.total);
        cache_save();
    }
}

static bool fetch_once(const app_config_t *cfg)
{
    char url[96];
    snprintf(url, sizeof(url), "https://github-contributions-api.jogruber.de/v4/%s",
             cfg->gh_user);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return false;

    bool ok = false;
    parse_reset();
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            int n;
            while ((n = esp_http_client_read(client, s_ps.chunk, sizeof(s_ps.chunk))) > 0) {
                parse_feed(s_ps.chunk, n);
            }
            ok = (s_ps.day > 0);
        } else {
            ESP_LOGW(TAG, "HTTP %d", esp_http_client_get_status_code(client));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

void app_github_fetch(void)
{
    const app_config_t *cfg = app_config_get();
    if (cfg->gh_user[0] == '\0') return;   // 未配置用户名,保留占位状态

    // TLS 建立要 ~40KB,开机脉冲时堆可能还没就绪:失败等 2 秒重试一次
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (fetch_once(cfg)) {
            parse_finish();
            return;
        }
        ESP_LOGW(TAG, "拉取失败(%d/2)", attempt);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
