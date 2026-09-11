// main/app_config.c —— 配置的 NVS 持久化与生效。
//
// 命名空间 "badge",每个字段一个键,便于门户/后续版本单独增删字段而不破坏旧数据。
// 缺失的键取默认值;读出后统一走 app_config_sanitize(),NVS 里的脏数据不会进入 UI。
// NVS 初始化失败不擦分区(会连带毁掉用户资料),只记录错误并以默认值运行。
#include "app_config.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_cfg";
// 命名空间带版本:换名字即弃用旧数据(上一版命名空间里的实验数据不再读)。
#define NVS_NS "badgebot_v2"

static app_config_t s_cfg;
static bool s_nvs_ready;

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t len) {
    size_t n = len;
    (void)nvs_get_str(h, key, dst, &n);   // 缺失或超长时不写 dst,保留默认值
}

static void get_u8(nvs_handle_t h, const char *key, uint8_t *dst) {
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *dst = v;
}

static void get_bool(nvs_handle_t h, const char *key, bool *dst) {
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *dst = (v != 0);
}

static esp_err_t load(void) {
    app_config_defaults(&s_cfg);
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   // 首次开机,还没保存过
    if (e != ESP_OK) return e;

    get_str (h, "name",  s_cfg.name,  sizeof(s_cfg.name));
    get_str (h, "org",   s_cfg.org,   sizeof(s_cfg.org));
    get_str (h, "title", s_cfg.title, sizeof(s_cfg.title));
    for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "qr%d", i);
        get_str(h, key, s_cfg.qr_text[i], sizeof(s_cfg.qr_text[i]));
        snprintf(key, sizeof(key), "ql%d", i);
        get_str(h, key, s_cfg.qr_label[i], sizeof(s_cfg.qr_label[i]));
    }
    get_u8  (h, "qrmask", &s_cfg.qr_mode);
    // 旧版单槽迁移:qr0 缺失且存在旧 qr_a 时继承为槽 0
    {
        char probe[8] = { 0 };
        size_t n = sizeof(probe);
        if (nvs_get_str(h, "qr0", probe, &n) != ESP_OK) {
            char legacy[APP_CFG_QR_LEN] = { 0 };
            n = sizeof(legacy);
            if (nvs_get_str(h, "qr_a", legacy, &n) == ESP_OK && legacy[0]) {
                strlcpy(s_cfg.qr_text[0], legacy, sizeof(s_cfg.qr_text[0]));
            }
        }
    }
    get_bool(h, "hide",  &s_cfg.hide_org_title);
    get_u8  (h, "layout",&s_cfg.layout);
    get_u8  (h, "laymask",&s_cfg.layout_mask);
    get_u8  (h, "lang",  &s_cfg.lang);
    get_u8  (h, "theme", &s_cfg.theme);
    get_u8  (h, "bl",    &s_cfg.brightness);
    get_u8  (h, "vol",   &s_cfg.volume);
    get_u8  (h, "offmin",&s_cfg.screen_off_min);
    get_bool(h, "boot",  &s_cfg.boot_badge);
    get_u8  (h, "hidmap",&s_cfg.hid_arrows);
    nvs_close(h);

    if (app_config_sanitize(&s_cfg)) {
        ESP_LOGW(TAG, "NVS 中的配置越界,已修正");
    }
    return ESP_OK;
}

static esp_err_t save(const app_config_t *cfg) {
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    // 任一写入失败就整体放弃提交,避免半新半旧的配置。
    if ((e = nvs_set_str(h, "name",  cfg->name))  == ESP_OK &&
        (e = nvs_set_str(h, "org",   cfg->org))   == ESP_OK &&
        (e = nvs_set_str(h, "title", cfg->title)) == ESP_OK &&
        (e = nvs_set_str(h, "qr0",  cfg->qr_text[0])) == ESP_OK &&
        (e = nvs_set_str(h, "qr1",  cfg->qr_text[1])) == ESP_OK &&
        (e = nvs_set_str(h, "qr2",  cfg->qr_text[2])) == ESP_OK &&
        (e = nvs_set_str(h, "qr3",  cfg->qr_text[3])) == ESP_OK &&
        (e = nvs_set_str(h, "ql0",  cfg->qr_label[0])) == ESP_OK &&
        (e = nvs_set_str(h, "ql1",  cfg->qr_label[1])) == ESP_OK &&
        (e = nvs_set_str(h, "ql2",  cfg->qr_label[2])) == ESP_OK &&
        (e = nvs_set_str(h, "ql3",  cfg->qr_label[3])) == ESP_OK &&
        (e = nvs_set_u8 (h, "qrmask", cfg->qr_mode)) == ESP_OK &&
        (e = nvs_set_u8 (h, "hide",  cfg->hide_org_title)) == ESP_OK &&
        (e = nvs_set_u8 (h, "layout",cfg->layout)) == ESP_OK &&
        (e = nvs_set_u8 (h, "laymask",cfg->layout_mask)) == ESP_OK &&
        (e = nvs_set_u8 (h, "lang",  cfg->lang)) == ESP_OK &&
        (e = nvs_set_u8 (h, "theme", cfg->theme)) == ESP_OK &&
        (e = nvs_set_u8 (h, "bl",    cfg->brightness)) == ESP_OK &&
        (e = nvs_set_u8 (h, "vol",   cfg->volume)) == ESP_OK &&
        (e = nvs_set_u8 (h, "offmin",cfg->screen_off_min)) == ESP_OK &&
        (e = nvs_set_u8 (h, "boot",  cfg->boot_badge)) == ESP_OK &&
        (e = nvs_set_u8 (h, "hidmap",cfg->hid_arrows)) == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

esp_err_t app_config_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS 分区需要迁移(%s),不自动擦除;本次以默认配置运行",
                 esp_err_to_name(e));
    } else if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败(%s),本次以默认配置运行", esp_err_to_name(e));
    } else {
        s_nvs_ready = true;
    }

    esp_err_t le = load();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "读取配置失败(%s),使用默认值", esp_err_to_name(le));
    }
    ESP_LOGI(TAG, "配置:name=\"%s\" layout=%s bl=%d vol=%d off=%dmin boot=%s",
             s_cfg.name, app_config_layout_name(s_cfg.layout), s_cfg.brightness,
             s_cfg.volume, s_cfg.screen_off_min, s_cfg.boot_badge ? "badge" : "menu");
    return s_nvs_ready ? ESP_OK : e;
}

const app_config_t *app_config_get(void) {
    return &s_cfg;
}

void app_config_apply(void) {
    bsp_display_backlight(s_cfg.brightness);
    bsp_audio_set_volume(s_cfg.volume);
}

// 恢复出厂:擦除应用命名空间(配置+宠物),回到默认值并立即生效。
// 注意:上传的头像/二维码图在 store 分区,由调用方另行删除。
void app_config_factory_reset(void)
{
    nvs_handle_t h;
    if (s_nvs_ready && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        if (e != ESP_OK) ESP_LOGW(TAG, "擦除配置失败(%s),仅恢复默认值", esp_err_to_name(e));
    }
    app_config_defaults(&s_cfg);
    app_config_apply();
    save(&s_cfg);
    ESP_LOGW(TAG, "已恢复出厂设置");
}

esp_err_t app_config_update(const app_config_t *cfg) {
    app_config_t next = *cfg;
    app_config_sanitize(&next);
    s_cfg = next;
    app_config_apply();
    esp_err_t e = save(&s_cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "配置保存失败(%s),本次开机内仍然生效", esp_err_to_name(e));
    }
    return e;
}
