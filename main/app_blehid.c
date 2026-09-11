// main/app_blehid.c —— BLE HID 翻页器:HID-over-GATT 键盘设备。
//
// 服务结构(Bluetooth HID profile):
//   HID 0x1812
//     ├ Report Map 0x2A4B      标准 8 字节键盘报表描述符
//     ├ HID Information 0x2A4A bcdHID 1.1
//     ├ HID Control Point 0x2A4C(挂起/恢复,忽略)
//     ├ Protocol Mode 0x2A4E   报表模式(默认)/引导模式
//     ├ Report 0x2A4D + Report Reference(1,Input)   报表模式下发键值
//     └ Boot Keyboard Input 0x2A22 + Report Reference(Boot,Input)
// 键值按 8 字节键盘报表 [修饰键, 保留, 键1..键6] 经 Notify 下发;
// 按下后 30ms 由 esp_timer 补发全零释放帧,避免依赖宿主自动释放。
// 配对:Just Works(无屏上口令输入需求),SM 安全连接 + 绑定;
// 密钥由 NimBLE 存储层写入 NVS(sdkconfig 已开 NVS_PERSIST),重开机免重配。
#include "app_blehid.h"
#include "app_blehid_keys.h"
#include "app_config.h"
#include "demo_radio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include <string.h>

// IDF 的 store/config 头只暴露读写删回调;初始化函数在例程里都是 extern 声明后调用
// (把 NimBLE 的密钥存储接到 NVS,配合 CONFIG_BT_NIMBLE_NVS_PERSIST 持久化绑定)。
int ble_store_config_init(void);

static const char *TAG = "app_blehid";
static const char *DEVICE_NAME = "BadgeBot";          // 宿主蓝牙列表里显示的名字

static volatile blehid_state_t s_state;
static volatile uint16_t s_conn_handle;               // 当前(唯一)连接句柄
static volatile bool s_encrypted;                     // 报表仅在加密链路上发送
static uint8_t s_addr_type;
static uint16_t s_rep_val_handle;                     // Report 0x2A4D 值句柄
static uint16_t s_boot_val_handle;                    // Boot Keyboard Input 值句柄
static uint8_t s_report[8];                           // 当前键值(读回/引导模式共用)

static bool s_initialized;
static SemaphoreHandle_t s_host_stopped;
static esp_timer_handle_t s_release_timer;            // 按键释放帧定时器

static int gap_event(struct ble_gap_event *event, void *arg);

// 标准 8 字节 boot 键盘报表描述符(HID 1.11 附录 B;带 Report Id 1)
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report Id (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Min (224)  修饰键
    0x29, 0xE7,  //   Usage Max (231)
    0x15, 0x00,  //   Logical Min (0)
    0x25, 0x01,  //   Logical Max (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Var, Abs)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Const, Array)  保留字节
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Min (0)
    0x25, 0x65,  //   Logical Max (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Min (0)
    0x29, 0x65,  //   Usage Max (101)
    0x81, 0x00,  //   Input (Data, Array)
    0xC0,        // End Collection
};

// ---- GATT 访问回调 ----

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    switch ((int)(intptr_t)arg) {
    case 0:                                          // Report Map
        return os_mbuf_append(ctxt->om, HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    case 1:                                          // HID Information
        { static const uint8_t info[4] = { 0x11, 0x01, 0x00, 0x02 };
          return os_mbuf_append(ctxt->om, info, sizeof(info)); }
    case 2:                                          // Protocol Mode(0=Boot,1=Report)
        { static const uint8_t proto = 0x01;
          return os_mbuf_append(ctxt->om, &proto, 1); }
    case 3:                                          // Report 读回
        return os_mbuf_append(ctxt->om, s_report, sizeof(s_report));
    case 4:                                          // Report Reference(Report,Input)
        { static const uint8_t ref_input[2] = { 0x01, 0x01 };
          return os_mbuf_append(ctxt->om, ref_input, 2); }
    case 5:                                          // Report Reference(Boot,Input)
        { static const uint8_t ref_boot[2] = { 0x00, 0x01 };
          return os_mbuf_append(ctxt->om, ref_boot, 2); }
    default:                                         // Control Point/Protocol 写:接受
        return 0;
    }
}

static const struct ble_gatt_svc_def HID_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4D),      // Report
              .access_cb = chr_access, .arg = (void *)3,
              .val_handle = &s_rep_val_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .access_cb = chr_access, .arg = (void *)4,
                    .att_flags = BLE_ATT_F_READ },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A22),      // Boot Keyboard Input
              .access_cb = chr_access, .arg = (void *)3,
              .val_handle = &s_boot_val_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .access_cb = chr_access, .arg = (void *)5,
                    .att_flags = BLE_ATT_F_READ },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B),      // Report Map
              .access_cb = chr_access, .arg = (void *)0,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4A),      // HID Information
              .access_cb = chr_access, .arg = (void *)1,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C),      // HID Control Point
              .access_cb = chr_access, .arg = (void *)6,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E),      // Protocol Mode
              .access_cb = chr_access, .arg = (void *)2,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 } },
    },
    { 0 },
};

// ---- 广播 ----

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = 961;                          // HID Keyboard
    fields.appearance_is_present = 1;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    static ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &svc_uuid;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    // 20~40ms 快广播:配对窗口期设备就在手边,不需要省电
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 32;                             // 32 * 0.625ms = 20ms
    params.itvl_max = 64;                             // 40ms
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) s_state = BLEHID_ADVERTISING;
    return rc;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_encrypted = false;
            s_state = BLEHID_CONNECTED;
            ESP_LOGI(TAG, "已连接,等待配对加密 handle=%u", s_conn_handle);
        } else {
            advertise();                              // 连接失败:继续广播
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 reason=%d,重新广播", event->disconnect.reason);
        s_encrypted = false;
        s_state = BLEHID_ADVERTISING;
        advertise();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:                    // 配对/加密完成
        if (event->enc_change.status == 0) {
            s_encrypted = true;
            s_state = BLEHID_READY;
            ESP_LOGI(TAG, "链路已加密,翻页键可发送");
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
    {
        // 同一设备用旧密钥重连(比如我们这边擦过 NVS):删掉旧绑定允许重新配对
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_state == BLEHID_ADVERTISING) advertise();
        return 0;
    default:
        return 0;
    }
}

// ---- 按键发送 ----

static void release_report(void *arg)
{
    (void)arg;
    if (s_state != BLEHID_READY) return;
    memset(s_report, 0, sizeof(s_report));
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
    if (om && s_rep_val_handle) ble_gatts_notify_custom(s_conn_handle, s_rep_val_handle, om);
    om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
    if (om && s_boot_val_handle) ble_gatts_notify_custom(s_conn_handle, s_boot_val_handle, om);
}

void app_blehid_send(int dir)
{
    if (s_state != BLEHID_READY) return;              // 未加密连接:按键无效
    uint8_t key = app_blehid_keycode(app_config_get()->hid_arrows, dir);
    memset(s_report, 0, sizeof(s_report));
    s_report[2] = key;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_rep_val_handle, om);
    if (rc == 0 && s_boot_val_handle) {
        om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
        if (om) ble_gatts_notify_custom(s_conn_handle, s_boot_val_handle, om);
    }
    // 30ms 后补发释放帧(定时间隔即注释里唯一的时间常量)
    esp_timer_stop(s_release_timer);
    esp_timer_start_once(s_release_timer, 30 * 1000);
}

// ---- 生命周期 ----

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 主机复位 reason=%d", reason);
    s_state = BLEHID_IDLE;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = advertise();
    if (rc != 0) {
        ESP_LOGE(TAG, "同步失败 rc=%d", rc);
        s_state = BLEHID_IDLE;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

esp_err_t app_blehid_start(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败 %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    s_host_stopped = xSemaphoreCreateBinary();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_gatts_count_cfg(HID_SVCS);
    ble_gatts_add_svcs(HID_SVCS);
    ble_store_config_init();                          // 绑定密钥存 NVS 的存储层

    const esp_timer_create_args_t timer_args = {
        .callback = release_report, .name = "hid-release",
    };
    if (!s_release_timer) esp_timer_create(&timer_args, &s_release_timer);

    // Just Works + 安全连接 + 绑定;密钥双端互发(加密 + 身份)
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY;
    ble_hs_cfg.sm_their_key_dist = BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void app_blehid_shutdown(void)
{
    if (!s_initialized) return;
    if (s_state == BLEHID_CONNECTED || s_state == BLEHID_READY) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc == 0 && s_host_stopped) {
        // 主机回调不碰 LVGL;页面 exit 持锁等待也不会成环
        xSemaphoreTake(s_host_stopped, portMAX_DELAY);
        nimble_port_deinit();
    } else {
        ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
    }
    if (s_host_stopped) { vSemaphoreDelete(s_host_stopped); s_host_stopped = NULL; }
    if (s_release_timer) { esp_timer_delete(s_release_timer); s_release_timer = NULL; }
    s_initialized = false;
    s_encrypted = false;
    s_state = BLEHID_IDLE;
}

blehid_state_t app_blehid_state(void)
{
    return s_state;
}
