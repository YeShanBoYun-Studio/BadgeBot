// main/app_store.c —— store 分区(FATFS)挂载。
// 分区位于 0x310000(280KB 空隙内),不触碰受保护的 cardid@0x356000。
// 存放:avatar.rgb565(96x96)、qr_b.rgb565(160x160)、notes.txt(M5)等上传资产。
#include "app_store.h"
#include "esp_log.h"
#include "wear_levelling.h"
#include "esp_vfs_fat.h"

static const char *TAG = "app_store";
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_ready;

esp_err_t app_store_init(void)
{
    if (s_ready) return ESP_OK;

    // max_files=2 足够(头像/二维码一次各写一个);format_on_fail 仅对全新分区发生。
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 2,
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/store", "store", &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "store 分区挂载失败(%s),上传类功能不可用", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    ESP_LOGI(TAG, "store 已挂载到 /store");
    return ESP_OK;
}

bool app_store_ready(void)
{
    return s_ready;
}
