// main/app_config.h —— 工牌应用配置:用户资料、显示与开机行为,NVS 持久化。
//
// 分两层实现,便于在主机上测试纯逻辑:
//   app_config_model.c  默认值、范围校验、设置页步进 —— 不依赖 ESP-IDF
//   app_config.c        NVS 读写、生效到背光/音量 —— 依赖 ESP-IDF(仅在目标上编译)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_CFG_STR_LEN     48    // UTF-8 字节上限(含 NUL),约 15 个汉字,够放姓名/公司/岗位
#define APP_CFG_QR_LEN      128   // 二维码槽文本上限(URL/任意文本)
#define APP_CFG_QR_SLOTS    4     // 二维码槽位数量(2x2 网格)
#define APP_CFG_QRLBL_LEN   24    // 二维码槽标签上限(如"微信"/"主页")
#define APP_CFG_URL_LEN     128   // 语音后端地址上限(http://host:port[/path])
#define APP_CFG_BL_MIN      10    // 背光下限(%),0 会让屏幕全黑、无法操作
#define APP_CFG_BL_MAX      100
#define APP_CFG_BL_STEP     10
#define APP_CFG_VOL_MAX     100
#define APP_CFG_VOL_STEP    10
#define APP_CFG_OFF_MAX     30    // 自动息屏上限(分钟);0 = 常亮

typedef enum {
    APP_LAYOUT_CARD = 0,          // A:名片(时间/头像/姓名/岗位/电量)
    APP_LAYOUT_QR,                // B:双二维码
    APP_LAYOUT_PET,               // C:像素宠物(拓麻歌子式养成)
    APP_LAYOUT_COUNT,
} app_layout_t;

#define APP_LAYOUT_ALL_MASK ((1 << APP_LAYOUT_COUNT) - 1)   // 全部布局开启

typedef struct {
    char    name[APP_CFG_STR_LEN];
    char    org[APP_CFG_STR_LEN];
    char    title[APP_CFG_STR_LEN];
    char    qr_text[APP_CFG_QR_SLOTS][APP_CFG_QR_LEN];  // 各槽生成文本(模式为"生成"时用)
    char    qr_label[APP_CFG_QR_SLOTS][APP_CFG_QRLBL_LEN]; // 各槽标签(如"微信")
    uint8_t qr_mode;            // 各槽内容来源:bit n = 1 表示上传图片,0 = 网页生成
    bool    hide_org_title;       // 主页不显示公司/岗位(默认隐藏)
    uint8_t layout;               // app_layout_t,开机默认布局
    uint8_t layout_mask;          // 布局开关位掩码,bit n = APP_LAYOUT_n;主页 ▲/▼ 只在开启的布局间切换
    uint8_t lang;                 // 界面语言:0 = 中文,1 = English
    uint8_t theme;                // 0 = 浅色,1 = 深色;换主题后新屏生效
    uint8_t brightness;           // APP_CFG_BL_MIN..APP_CFG_BL_MAX
    uint8_t volume;               // 0..APP_CFG_VOL_MAX
    uint8_t screen_off_min;       // 0..APP_CFG_OFF_MAX,0 = 常亮
    bool    boot_badge;           // 开机直接进工牌主页,否则进菜单
    uint8_t hid_arrows;           // 翻页器键位:0 = PgUp/PgDn,1 = 左/右方向键
    char    voice_url[APP_CFG_URL_LEN]; // 语音后端(http://),空 = 语音功能未配置
} app_config_t;

// ---- 纯逻辑(app_config_model.c) ----
void    app_config_defaults(app_config_t *cfg);
// 越界值拉回合法范围、字符串补 NUL;返回是否改动过(改动过说明来源不可信,需回写)。
bool    app_config_sanitize(app_config_t *cfg);
// 设置页步进:dir=+1 下一档、-1 上一档,越界回绕。
uint8_t app_config_step_brightness(uint8_t cur, int dir);
uint8_t app_config_step_volume(uint8_t cur, int dir);
uint8_t app_config_step_screen_off(uint8_t cur, int dir);
// 在 layout_mask 允许的布局中从 cur 出发步进(dir=+1 下一个);无可用布局时返回 cur。
uint8_t app_config_next_layout(uint8_t cur, uint8_t mask, int dir);
const char *app_config_layout_name(uint8_t layout);

// ---- 目标端(app_config.c) ----
#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t app_config_init(void);                        // 初始化 NVS 并载入配置(缺失键取默认值)
const app_config_t *app_config_get(void);               // 当前生效配置
esp_err_t app_config_update(const app_config_t *cfg);   // 校验、写入 NVS、并立即生效
void      app_config_apply(void);                       // 把当前配置作用到背光/音量(外设初始化完后调一次)
void      app_config_factory_reset(void);               // 恢复出厂:擦配置+宠物,回到默认值
#endif
