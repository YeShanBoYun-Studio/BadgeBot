// main/app_github.c —— GitHub 提交热力图。
//
// 数据源:github-contributions-api.jogruber.de/v4/<user>(免费公开接口,返回
// 最近一年的逐日 level 0..4)。host 是编译期常量,用户名在配置层已过滤为
// [A-Za-z0-9-_],不存在任意 URL 注入。
//
// 拉取发生在 Wi-Fi 脉冲窗口内(已连接状态),TLS + 全量 JSON 峰值约 70KB 堆,
// 此时显示/音频缓冲竞争最小;失败静默保留旧缓存,页面永远有内容可画。
#include "app_github.h"
#include "app_config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_gh";
#define NVS_NS "badgebot_v2"
#define FETCH_BUF_LEN (24 * 1024)
#define MAX_DAYS 400

static uint8_t s_levels[GH_DAYS];        // 0..4,255 = 无数据
static int s_total;
static bool s_ready;

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

// 从整段 JSON 中顺序抠出 "level":N 与 "count":N(免建 cJSON 树,内存友好)
static void parse_levels(char *body, int body_len)
{
    memset(s_levels, 255, sizeof(s_levels));
    int day = 0, total = 0;
    const char *p = body;
    const char *end = body + body_len;
    while (p < end && day < MAX_DAYS) {
        const char *lv = strstr(p, "\"level\":");
        if (!lv) break;
        int level = atoi(lv + 8);
        if (level < 0) level = 0;
        if (level > 4) level = 4;
        s_levels[day++] = (uint8_t)level;

        const char *cnt = strstr(lv, "\"count\":");
        if (cnt && cnt < (lv + 40)) {          // count 在同一对象内、紧邻 level 之前
            total += atoi(cnt + 8);
        }
        p = lv + 8;
    }
    if (day > 0) {
        s_total = total;
        s_ready = true;
        ESP_LOGI(TAG, "热力图解析: %d 天,共 %d 次", day, total);
        cache_save();
    }
}

void app_github_fetch(void)
{
    const app_config_t *cfg = app_config_get();
    if (cfg->gh_user[0] == '\0') return;   // 未配置用户名,保留占位状态

    char *body = malloc(FETCH_BUF_LEN);
    if (!body) {
        ESP_LOGW(TAG, "拉取缓冲分配失败");
        return;
    }

    char url[96];
    snprintf(url, sizeof(url), "https://github-contributions-api.jogruber.de/v4/%s",
             cfg->gh_user);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(body);
        return;
    }
    esp_err_t e = esp_http_client_open(client, 0);
    int len = -1;
    if (e == ESP_OK) {
        len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int received = 0;
        if (status == 200) {
            while (received < FETCH_BUF_LEN - 1) {
                int n = esp_http_client_read(client, body + received, FETCH_BUF_LEN - 1 - received);
                if (n <= 0) break;
                received += n;
            }
            body[received] = '\0';
            len = received;
        } else {
            ESP_LOGW(TAG, "HTTP %d", status);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (len > 0) parse_levels(body, len);
    else ESP_LOGW(TAG, "拉取失败");
    free(body);
}
