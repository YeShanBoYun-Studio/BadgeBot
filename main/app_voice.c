// main/app_voice.c —— V2 语音 worker:录音边录边传(分块上传,不吃大块 RAM),
// 应答解析在 app_voice_model(有主机测试)。与 LVGL 无关,页面只轮询状态。
#include "app_voice.h"
#include "app_config.h"
#include "app_voice_model.h"
#include "app_wifi.h"
#include "bsp_audio.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_voice";

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 1024                      // 64ms @16k;上传粒度与缓冲都可控
#define NET_WAIT_MS   20000                     // 等按需联网的最长时间
#define HTTP_TIMEOUT_MS 10000

static TaskHandle_t s_task;
static volatile bool s_start_req;               // 音频任务轮询(同 demo_audio 模式)
static volatile bool s_stop_req;                // 录音/联网阶段提前收尾
static volatile voice_state_t s_state = VOICE_IDLE;
static volatile voice_err_t s_err;
static volatile uint32_t s_rec_start_ms;
static char s_text[VOICE_TEXT_CAP];             // worker 写、页面在 DONE 态取走

static bool busy(voice_state_t st)
{
    return st == VOICE_WAIT_NET || st == VOICE_REC || st == VOICE_SEND;
}

static void set_state(voice_state_t st, voice_err_t err)
{
    s_err = err;
    s_state = st;
}

// 等按需联网就绪;s_stop_req 置位时取消。返回是否已连上。
static bool wait_network(void)
{
    if (app_wifi_hold_open() != ESP_OK) {
        ESP_LOGE(TAG, "无法请求联网(配网中或 Wi-Fi 未就绪)");
        return false;
    }
    int waited = 0;
    while (!app_wifi_connected() && waited < NET_WAIT_MS) {
        if (s_stop_req) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return app_wifi_connected();
}

// 录音并分块上传,取回应答;返回 VOICE_OK 表示识别文本已写入 s_text。
static voice_err_t record_and_send(const char *endpoint)
{
    esp_err_t err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败: %s", esp_err_to_name(err));
        return VOICE_ERR_MIC;
    }

    esp_http_client_config_t hc = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // 局域网明文通道(设计决定见 app_voice_model.h):不存在证书校验问题
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        return VOICE_ERR_CONN;
    }
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    voice_err_t ret = VOICE_OK;
    int16_t *chunk = malloc(sizeof(int16_t) * CHUNK_SAMPLES);
    if (!chunk) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "录音分块缓冲分配失败");
        return VOICE_ERR_MIC;
    }

    // write_len = -1 → chunked 编码:总长未知也能边录边传
    if (esp_http_client_open(client, -1) != ESP_OK) {
        ESP_LOGE(TAG, "连接语音后端失败");
        free(chunk);
        esp_http_client_cleanup(client);
        return VOICE_ERR_CONN;
    }

    s_rec_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        if (bsp_audio_read(chunk, sizeof(int16_t) * CHUNK_SAMPLES) != ESP_OK) {
            ESP_LOGE(TAG, "麦克风读取失败");
            ret = VOICE_ERR_MIC;
            break;
        }
        if (esp_http_client_write(client, (const char *)chunk,
                                  sizeof(int16_t) * CHUNK_SAMPLES) < 0) {
            ESP_LOGE(TAG, "上传中断");
            ret = VOICE_ERR_SEND;
            break;
        }
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - s_rec_start_ms;
        if (s_stop_req || elapsed >= VOICE_MAX_RECORD_SEC * 1000) {
            ESP_LOGI(TAG, "录音结束:%ums(%s)", (unsigned)elapsed,
                     s_stop_req ? "手动" : "到上限");
            break;
        }
    }

    if (ret == VOICE_OK) {
        // 收尾:fetch_headers 补发 chunked 终止块并读回应答头
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        char resp[512];
        int n = 0;
        while (n < (int)sizeof(resp) - 1) {
            int r = esp_http_client_read(client, resp + n, 1);
            if (r <= 0) break;
            n++;
        }
        resp[n] = '\0';
        if (status != 200) {
            ESP_LOGE(TAG, "后端返回 HTTP %d: %.80s", status, resp);
            ret = VOICE_ERR_HTTP;
        } else if (!voice_result_text(resp, s_text, sizeof(s_text))) {
            ESP_LOGE(TAG, "应答里没有文本: %.80s", resp);
            ret = VOICE_ERR_EMPTY;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk);
    return ret;
}

static void voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_start_req) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_start_req = false;
        s_stop_req = false;
        s_text[0] = '\0';
        set_state(VOICE_WAIT_NET, VOICE_OK);

        const app_config_t *cfg = app_config_get();
        char endpoint[APP_CFG_URL_LEN + 40];
        if (!voice_url_valid(cfg->voice_url) ||
            !voice_endpoint(cfg->voice_url, SAMPLE_RATE, 16, 1, endpoint, sizeof(endpoint))) {
            set_state(VOICE_ERR, VOICE_ERR_NOURL);
            continue;
        }
        if (!wait_network()) {
            set_state(s_stop_req ? VOICE_IDLE : VOICE_ERR, VOICE_ERR_NET);
            app_wifi_hold_close();
            continue;
        }

        set_state(VOICE_REC, VOICE_OK);
        voice_err_t r = record_and_send(endpoint);
        if (r == VOICE_OK) {
            // 亮一下"识别中",长句时页面不至于从录音直接跳结果
            set_state(VOICE_SEND, VOICE_OK);
            vTaskDelay(pdMS_TO_TICKS(50));
            set_state(VOICE_DONE, VOICE_OK);
            ESP_LOGI(TAG, "识别完成:%.60s", s_text);
        } else {
            set_state(VOICE_ERR, r);
        }
        app_wifi_hold_close();
    }
}

void app_voice_start(void)
{
    if (busy(s_state)) return;
    if (!s_task) {
        if (xTaskCreate(voice_task, "app_voice", 12288, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "语音任务创建失败");
            set_state(VOICE_ERR, VOICE_ERR_MIC);
            return;
        }
    }
    s_stop_req = false;
    s_start_req = true;
}

void app_voice_stop(void)
{
    if (s_state == VOICE_REC || s_state == VOICE_WAIT_NET) s_stop_req = true;
}

voice_state_t app_voice_state(void) { return s_state; }
voice_err_t app_voice_error(void) { return s_err; }
uint32_t app_voice_elapsed_ms(void)
{
    return s_state == VOICE_REC
               ? (uint32_t)(esp_timer_get_time() / 1000) - s_rec_start_ms
               : 0;
}

bool app_voice_take_text(char *out, size_t cap)
{
    if (s_state != VOICE_DONE || !out) return false;
    snprintf(out, cap, "%s", s_text);
    s_text[0] = '\0';
    return out[0] != '\0';
}
