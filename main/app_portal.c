// main/app_portal.c —— 配网门户:内嵌配置页 + 配置/Wi-Fi/上传 API。
//
// 页面是手机浏览器打开的 http://192.168.4.1;接口:
//   GET  /api/config  当前配置(不含 Wi-Fi 密码;附带热点剩余毫秒数,页面心跳用)
//   POST /api/config  保存名片资料(name/org/title/hide/qr_text[4]/qr_label[4]/qr_mode/lang)
//   POST /api/wifi    保存 STA 凭据(写入 NVS 并延长热点窗口,让用户继续上传)
//   POST /api/avatar  头像(96x96 RGB565 原始字节,body 为 0 清除)
//   POST /api/qr0..3  二维码槽 0..3 的上传图(128x128 1bpp,body 为 0 清除);
//                     每槽内容来源由 qr_mode 位图选择:bit n = 1 用上传图,0 用网页生成文本
//   GET/POST /api/notes 提词稿(notes.txt,UTF-8 纯文本,一行 = 一段,上限 32KB);
//                     页面支持粘贴 / .txt 读入 / .pptx 备注提取(浏览器端解包,零依赖)
//   POST /api/done    用户点「完成并联网」:立即联网校时;热点保持开放至窗口结束
// 图片由页面 JS 裁剪:默认居中正方形选区,可拖动/缩放;固件不做图片解码。
// 注意:store 分区曾因 FATFS 长文件名未启用(CONFIG_FATFS_LFN_NONE)导致
// fopen("avatar.rgb565") 失败(8.3 短名放不下 6 字符扩展名),页面表现为
// "写入失败";而 qr_b.img 恰好合规所以能传成功。现已在 sdkconfig 打开 LFN。
#include "app_portal.h"
#include "app_config.h"
#include "app_store.h"
#include "app_wifi.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_portal";

/* 内嵌配置页(默认中文,页面右上角可切 English)。为省转义,HTML/JS 里统一用单引号,
 * C 字符串内不出现 ASCII 双引号(引号用「」或全角)。
 * 注意:input 的 id 不能叫 name/title——那会撞上 window.name/window.title 内置属性。
 * 每个可见标签都有 id:JS 端按 D 字典做中/英切换;所有请求都有成功(绿)/失败(红)回显。 */
#include "portal_html_gz.h"


static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

// 联网探测应答:Android(generate_204)与 iOS/macOS(hotspot-detect)各一份。
static esp_err_t probe_204(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t probe_ios(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "Success", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_index(httpd_req_t *req)
{
    // 门户页面以 gzip 整体发送(源文件 main/portal.html,构建物 portal_html_gz.h):
    // 19KB→7KB 基本一次 TCP 窗口送达,不再出现"页面截断、按钮全死"
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)PORTAL_HTML_GZ, PORTAL_HTML_GZ_LEN);
}

static esp_err_t get_config(httpd_req_t *req)
{
    const app_config_t *c = app_config_get();
    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    char ssid[(sizeof sta.sta.ssid) + 1] = { 0 };
    memcpy(ssid, sta.sta.ssid, sizeof(sta.sta.ssid));   // SSID 不保证 NUL 结尾

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", c->name);
    cJSON_AddStringToObject(j, "org", c->org);
    cJSON_AddStringToObject(j, "title", c->title);
    cJSON *qt = cJSON_AddArrayToObject(j, "qr_text");
    cJSON *ql = cJSON_AddArrayToObject(j, "qr_label");
    for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
        cJSON_AddItemToArray(qt, cJSON_CreateString(c->qr_text[i]));
        cJSON_AddItemToArray(ql, cJSON_CreateString(c->qr_label[i]));
    }
    cJSON_AddNumberToObject(j, "qr_mode", c->qr_mode);
    cJSON_AddStringToObject(j, "voice_url", c->voice_url);
    cJSON_AddBoolToObject(j, "hide", c->hide_org_title);
    cJSON_AddNumberToObject(j, "layout", c->layout);
    cJSON_AddNumberToObject(j, "theme", c->theme);
    cJSON_AddNumberToObject(j, "lang", c->lang);
    cJSON_AddStringToObject(j, "sta_ssid", ssid);
    cJSON_AddNumberToObject(j, "remaining", (double)app_wifi_portal_remaining_ms());
    const char *body = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)body);
    cJSON_Delete(j);
    return ESP_OK;
}

// 读 POST body(上限 1KB,足够本门户的所有表单)
static char *read_body(httpd_req_t *req)
{
    static char buf[1024];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) return NULL;
        got += n;
    }
    buf[total] = '\0';
    return buf;
}

static esp_err_t post_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_text(req, "bad request");
    cJSON *j = cJSON_Parse(body);
    if (!j) return send_text(req, "bad json");

    app_config_t c = *app_config_get();
    cJSON *item;
    if ((item = cJSON_GetObjectItem(j, "name"))  && cJSON_IsString(item))
        strlcpy(c.name, item->valuestring, sizeof(c.name));
    if ((item = cJSON_GetObjectItem(j, "org"))   && cJSON_IsString(item))
        strlcpy(c.org, item->valuestring, sizeof(c.org));
    if ((item = cJSON_GetObjectItem(j, "title")) && cJSON_IsString(item))
        strlcpy(c.title, item->valuestring, sizeof(c.title));
    cJSON *qt = cJSON_GetObjectItem(j, "qr_text");
    cJSON *ql = cJSON_GetObjectItem(j, "qr_label");
    if (cJSON_IsArray(qt)) {
        for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
            cJSON *s = cJSON_GetArrayItem(qt, i);
            if (cJSON_IsString(s))
                strlcpy(c.qr_text[i], s->valuestring, sizeof(c.qr_text[i]));
        }
    }
    if (cJSON_IsArray(ql)) {
        for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
            cJSON *s = cJSON_GetArrayItem(ql, i);
            if (cJSON_IsString(s))
                strlcpy(c.qr_label[i], s->valuestring, sizeof(c.qr_label[i]));
        }
    }
    if ((item = cJSON_GetObjectItem(j, "voice_url")) && cJSON_IsString(item))
        strlcpy(c.voice_url, item->valuestring, sizeof(c.voice_url));
    if ((item = cJSON_GetObjectItem(j, "qr_mode")) && cJSON_IsNumber(item))
        c.qr_mode = (uint8_t)(item->valueint & 0x0F);
    if ((item = cJSON_GetObjectItem(j, "hide"))  && cJSON_IsBool(item))
        c.hide_org_title = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(j, "lang"))  && cJSON_IsNumber(item))
        c.lang = (uint8_t)(item->valueint != 0);
    cJSON_Delete(j);

    app_config_update(&c);
    ESP_LOGI(TAG, "门户保存名片资料");
    return send_text(req, "已保存 / Saved");
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_text(req, "bad request");
    cJSON *j = cJSON_Parse(body);
    if (!j) return send_text(req, "bad json");

    cJSON *sid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pwd = cJSON_GetObjectItem(j, "pass");
    if (!cJSON_IsString(sid) || sid->valuestring[0] == '\0') {
        cJSON_Delete(j);
        return send_text(req, "SSID 不能为空 / SSID required");
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, sid->valuestring, sizeof(sta.sta.ssid));
    if (cJSON_IsString(pwd)) {
        strlcpy((char *)sta.sta.password, pwd->valuestring, sizeof(sta.sta.password));
    }
    cJSON_Delete(j);

    // 存储模式是 FLASH:这里写入即持久化到 NVS,之后每次脉冲零配置连接。
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 凭据保存失败: %s", esp_err_to_name(e));
        return send_text(req, "保存失败 / Save failed");
    }
    ESP_LOGI(TAG, "门户保存 Wi-Fi: %s", sta.sta.ssid);
    // 保存后热点保持开启,让用户继续上传图片/保存资料;点"完成并联网"或超时才关
    app_wifi_portal_extend(120000);
    return send_text(req, "已保存,热点保持开启 / Saved, hotspot stays on");
}

static esp_err_t post_done(httpd_req_t *req)
{
    ESP_LOGI(TAG, "门户触发联网校时(热点保持开放)");
    app_wifi_portal_sync_now();   // 只校时不关热点;窗口倒计时结束才关闭
    return send_text(req, "已开始联网校时,热点保持开放 / Syncing, hotspot stays open");
}

// 流式接收 body 直写 /store 文件;content_len 为 0 表示删除该资产。
// 头像 18KB、二维码图 2KB 都超出通用读缓冲,不能一次 recv。
// fopen 失败时打印 errno:若再现"写入失败",优先怀疑 flash/挂载状态而非长度。
static esp_err_t upload_asset(httpd_req_t *req, const char *name, long expected_len)
{
    if (!app_store_ready()) {
        ESP_LOGW(TAG, "上传 %s 被拒:存储不可用", name);
        return send_text(req, "存储不可用 / Storage unavailable");
    }

    if (req->content_len == 0) {
        app_store_write(name, NULL, 0);
        return send_text(req, "已清除 / Cleared");
    }
    if (req->content_len != expected_len) {
        ESP_LOGW(TAG, "上传 %s 长度不符: got %ld want %ld",
                 name, (long)req->content_len, expected_len);
        return send_text(req, "数据长度不符 / Bad length");
    }

    char path[48];
    snprintf(path, sizeof(path), "/store/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "上传 %s 打开文件失败(errno=%d)", path, errno);
        return send_text(req, "写入失败 / Write failed");
    }

    char buf[512];
    int remaining = req->content_len, failed = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) { failed = 1; break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { failed = 1; break; }
        remaining -= n;
    }
    fclose(f);
    if (failed) {
        ESP_LOGW(TAG, "上传 %s 中断,已回滚(剩 %d 字节)", name, remaining);
        app_store_write(name, NULL, 0);   // 半截文件直接清掉
        return send_text(req, "接收中断,已回滚 / Interrupted, rolled back");
    }
    ESP_LOGI(TAG, "资产 %s 已上传(%ld 字节)", name, expected_len);
    return send_text(req, "已上传 / Uploaded");
}

static esp_err_t post_avatar(httpd_req_t *req)
{
    return upload_asset(req, "avatar.rgb565", 96 * 96 * 2);
}

// 槽 0..3 的上传图共用同一处理:文件名 qr0.img..qr3.img,128x128 1bpp = 2048 字节
static esp_err_t post_qr0(httpd_req_t *req) { return upload_asset(req, "qr0.img", 128 * 128 / 8); }
static esp_err_t post_qr1(httpd_req_t *req) { return upload_asset(req, "qr1.img", 128 * 128 / 8); }
static esp_err_t post_qr2(httpd_req_t *req) { return upload_asset(req, "qr2.img", 128 * 128 / 8); }
static esp_err_t post_qr3(httpd_req_t *req) { return upload_asset(req, "qr3.img", 128 * 128 / 8); }

// ---- 提词稿(notes.txt:UTF-8 纯文本,一行 = 一段,上限 32KB) ----

#define NOTES_MAX_BYTES 32768

static esp_err_t post_notes(httpd_req_t *req)
{
    if (!app_store_ready()) {
        return send_text(req, "存储不可用 / Storage unavailable");
    }
    int len = req->content_len;
    if (len < 0 || len > NOTES_MAX_BYTES) {
        return send_text(req, "太长(上限 32KB) / Too long (32KB max)");
    }
    if (len == 0) {
        app_store_write("notes.txt", NULL, 0);
        return send_text(req, "已清除 / Cleared");
    }
    // 32KB 放不进栈上缓冲:流式 recv 直写文件,半截即回滚
    FILE *f = fopen("/store/notes.txt", "wb");
    if (!f) {
        ESP_LOGW(TAG, "notes.txt 打开失败(errno=%d)", errno);
        return send_text(req, "写入失败 / Write failed");
    }
    char buf[512];
    int remaining = len, got = 0, failed = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) { failed = 1; break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { failed = 1; break; }
        remaining -= n;
        got += n;
    }
    fclose(f);
    if (failed) {
        ESP_LOGW(TAG, "notes.txt 上传中断,已回滚(收 %d/%d)", got, len);
        app_store_write("notes.txt", NULL, 0);
        return send_text(req, "接收中断,已回滚 / Interrupted, rolled back");
    }
    ESP_LOGI(TAG, "提词稿已保存(%d 字节)", len);
    return send_text(req, "已保存 / Saved");
}

static esp_err_t get_notes(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (!app_store_ready()) return httpd_resp_send(req, "", 0);
    FILE *f = fopen("/store/notes.txt", "rb");
    if (!f) return httpd_resp_send(req, "", 0);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/",          .method = HTTP_GET,  .handler = get_index  },
    { .uri = "/api/config",.method = HTTP_GET,  .handler = get_config },
    { .uri = "/api/config",.method = HTTP_POST, .handler = post_config},
    { .uri = "/api/wifi",  .method = HTTP_POST, .handler = post_wifi  },
    { .uri = "/api/done",  .method = HTTP_POST, .handler = post_done  },
    { .uri = "/api/avatar",.method = HTTP_POST, .handler = post_avatar},
    { .uri = "/api/qr0",   .method = HTTP_POST, .handler = post_qr0   },
    { .uri = "/api/qr1",   .method = HTTP_POST, .handler = post_qr1   },
    { .uri = "/api/qr2",   .method = HTTP_POST, .handler = post_qr2   },
    { .uri = "/api/qr3",   .method = HTTP_POST, .handler = post_qr3   },
    { .uri = "/api/notes", .method = HTTP_GET,  .handler = get_notes  },
    { .uri = "/api/notes", .method = HTTP_POST, .handler = post_notes },
    // 手机系统联网探测:让系统把这网络当"可用",否则安卓会深度休眠网卡,
    // 下行 TCP 全部堵死(EAGAIN)、重连四次握手超时(手机报"密码错误")。
    // 代价是浏览器不会自动弹配网页,手动开 http://192.168.4.1 即可。
    { .uri = "/generate_204",       .method = HTTP_GET, .handler = probe_204 },
    { .uri = "/hotspot-detect.html",.method = HTTP_GET, .handler = probe_ios },
};

static httpd_handle_t s_server;

// 未匹配路径 → 302 到门户首页。手机用 /generate_204(Android)等探测判断
// 是否强制门户:返回 404 会被判定"无互联网",浏览器随后把流量切回蜂窝,
// 用户就永远打不开 192.168.4.1;返回 302 才会触发系统弹出门户登录页。
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Portal: http://192.168.4.1/", HTTPD_RESP_USE_STRLEN);
}

esp_err_t app_portal_http_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;   // 上传路径(FATFS 写入 + recv 循环)在默认 4KB 栈上偏紧
    // 门户 14 个 URI 处理器,默认上限 8 会让后 6 个静默注册失败
    cfg.max_uri_handlers = 16;
    // 堆只有 ~35KB:每个 TCP 连接的收发缓冲都从堆里出。安卓会一口气开多个连接,
    // 只留 3 个 + LRU 淘汰,防止缓冲把堆吃穿(表现为发送 EAGAIN、Wi-Fi 莫名掉线)
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) return e;
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_register_uri_handler(s_server, &URIS[i]);
    }
    e = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_404);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "404 跳转注册失败: %s(门户仍可用,但手机不会自动弹出)", esp_err_to_name(e));
    }
    return ESP_OK;
}

void app_portal_http_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
