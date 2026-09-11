// main/app_pet.c —— 宠物状态的 NVS 持久化(目标端)。
// 状态存 "badgebot_v2" 命名空间的 "pet" blob;时间源为系统 epoch(SNTP 校时后有效)。
#include "app_pet.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "app_pet";
#define NVS_NS "badgebot_v2"
#define NVS_KEY "pet"

static pet_state_t s_pet;

void app_pet_load(void)
{
    memset(&s_pet, 0, sizeof(s_pet));
    s_pet.hatch_epoch = UINT32_MAX;              // 未初始化标记

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_pet);
    esp_err_t e = nvs_get_blob(h, NVS_KEY, &s_pet, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(s_pet) || s_pet.hatch_epoch == 0) {
        s_pet.hatch_epoch = UINT32_MAX;
        return;
    }
    ESP_LOGI(TAG, "宠物已载入:age=%umin hunger=%u happy=%u",
             s_pet.age_min, s_pet.hunger, s_pet.happy);
}

const pet_state_t *app_pet_get(void) { return &s_pet; }

pet_state_t *app_pet_mut(void) { return &s_pet; }

bool app_pet_initialized(void) { return s_pet.hatch_epoch != UINT32_MAX; }

void app_pet_birth(uint32_t now)
{
    if (app_pet_initialized()) return;
    pet_model_init(&s_pet, now);
    app_pet_save();
    ESP_LOGI(TAG, "生蛋!孵化倒计时 2 分钟");
}

void app_pet_reset(void)
{
    s_pet.hatch_epoch = UINT32_MAX;
    memset(&s_pet, 0, sizeof(s_pet));
    s_pet.hatch_epoch = UINT32_MAX;              // 恢复出厂后回到"未开始养"
}

void app_pet_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, NVS_KEY, &s_pet, sizeof(s_pet));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "宠物状态保存失败(%s)", esp_err_to_name(e));
}
