// tests/test_app_config.c —— app_config 纯逻辑(默认值/校验/设置页步进)的主机测试。
// 只编译 main/app_config_model.c,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>
#include "app_config.h"

static void test_defaults(void)
{
    app_config_t c;
    app_config_defaults(&c);
    assert(c.layout == APP_LAYOUT_CARD);
    assert(c.theme == 0);
    assert(c.brightness == APP_CFG_BL_MAX);
    assert(c.volume == 60);
    assert(c.screen_off_min == 5);
    assert(c.boot_badge);
    assert(c.hide_org_title);                  // 公司/岗位默认隐藏
    assert(strcmp(c.name, "BadgeBot") == 0);   // 默认名不绑定任何用户身份
    assert(c.org[0] == '\0' && c.title[0] == '\0');
    assert(c.qr_text[0][0] != '\0');           // 槽 1 有可演示的默认内容,其余槽留空
    for (int i = 1; i < APP_CFG_QR_SLOTS; i++) {
        assert(c.qr_text[i][0] == '\0');
        assert(c.qr_label[i][0] == '\0');
    }
    assert(c.qr_mode == 0);                    // 默认全部为"网页生成"
    assert(!app_config_sanitize(&c));          // 默认值本身必须合法
}

static void test_sanitize(void)
{
    app_config_t c;
    app_config_defaults(&c);
    c.brightness = 200;                          // 越上界
    c.volume = 255;
    c.layout = 9;                                // 未知布局
    c.theme = 7;                                 // 未知主题
    c.screen_off_min = 99;
    memset(c.org, 'A', sizeof(c.org));           // 填满、无 NUL
    assert(app_config_sanitize(&c));
    assert(c.brightness == APP_CFG_BL_MAX);
    assert(c.volume == APP_CFG_VOL_MAX);
    assert(c.layout == APP_LAYOUT_CARD);
    assert(c.theme == 0);
    assert(c.screen_off_min == APP_CFG_OFF_MAX);
    assert(c.org[APP_CFG_STR_LEN - 1] == '\0');
    assert(strlen(c.org) == APP_CFG_STR_LEN - 1);

    app_config_defaults(&c);
    c.brightness = 0;                            // 越下界:不能让屏幕全黑
    assert(app_config_sanitize(&c));
    assert(c.brightness == APP_CFG_BL_MIN);

    // 二维码 4 槽:qr_mode 位图越界回退为全"网页生成",字符串保证 NUL 结尾
    app_config_defaults(&c);
    c.qr_mode = 0xFF;
    memset(c.qr_text[0], 'B', sizeof(c.qr_text[0]));
    memset(c.qr_label[3], 'C', sizeof(c.qr_label[3]));
    assert(app_config_sanitize(&c));
    assert(c.qr_mode == 0);
    assert(c.qr_text[0][APP_CFG_QR_LEN - 1] == '\0');
    assert(strlen(c.qr_text[0]) == APP_CFG_QR_LEN - 1);
    assert(c.qr_label[3][APP_CFG_QRLBL_LEN - 1] == '\0');
    assert(strlen(c.qr_label[3]) == APP_CFG_QRLBL_LEN - 1);
}

static void test_steps(void)
{
    // 亮度:10..100 步进 10,两端回绕
    assert(app_config_step_brightness(30, +1) == 40);
    assert(app_config_step_brightness(100, +1) == 10);
    assert(app_config_step_brightness(10, -1) == 100);
    // 任意值先吸附到网格再步进
    assert(app_config_step_brightness(25, +1) == 40);
    assert(app_config_step_brightness(24, +1) == 30);
    assert(app_config_step_brightness(200, +1) == 10);

    // 音量:0..100 步进 10
    assert(app_config_step_volume(60, +1) == 70);
    assert(app_config_step_volume(100, +1) == 0);
    assert(app_config_step_volume(0, -1) == 100);

    // 息屏:0/1/5/10/30 循环;非档位值落到不大于它的档
    assert(app_config_step_screen_off(5, +1) == 10);
    assert(app_config_step_screen_off(5, -1) == 1);
    assert(app_config_step_screen_off(30, +1) == 0);
    assert(app_config_step_screen_off(0, -1) == 30);
    assert(app_config_step_screen_off(7, +1) == 10);
    assert(app_config_step_screen_off(7, -1) == 1);

    assert(strcmp(app_config_layout_name(APP_LAYOUT_CARD), "CARD") == 0);
    assert(strcmp(app_config_layout_name(APP_LAYOUT_QR), "QR") == 0);
    assert(strcmp(app_config_layout_name(APP_LAYOUT_PET), "PET") == 0);
    assert(strcmp(app_config_layout_name(APP_LAYOUT_COUNT), "?") == 0);
}

static void test_layout_mask(void)
{
    app_config_t c;
    app_config_defaults(&c);
    assert(c.layout_mask == APP_LAYOUT_ALL_MASK);
    assert(c.lang == 0);
    assert(!app_config_sanitize(&c));

    // 默认布局被禁用时挪到第一个开启的布局
    c.layout = APP_LAYOUT_CARD;
    c.layout_mask = (1 << APP_LAYOUT_QR) | (1 << APP_LAYOUT_PET);
    assert(app_config_sanitize(&c));
    assert(c.layout == APP_LAYOUT_QR);
    assert(!app_config_sanitize(&c));

    // 掩码为空/越界回退为全开
    c.layout_mask = 0;
    assert(app_config_sanitize(&c));
    assert(c.layout_mask == APP_LAYOUT_ALL_MASK);
    c.layout_mask = 0xFF;
    assert(app_config_sanitize(&c));
    assert(c.layout_mask == APP_LAYOUT_ALL_MASK);

    // 非法语言回中文
    c.lang = 9;
    assert(app_config_sanitize(&c));
    assert(c.lang == 0);

    // next_layout:只在掩码允许的布局间循环
    uint8_t m = (uint8_t)((1 << APP_LAYOUT_CARD) | (1 << APP_LAYOUT_PET));
    assert(app_config_next_layout(APP_LAYOUT_CARD, m, +1) == APP_LAYOUT_PET);
    assert(app_config_next_layout(APP_LAYOUT_PET, m, +1) == APP_LAYOUT_CARD);
    assert(app_config_next_layout(APP_LAYOUT_CARD, m, -1) == APP_LAYOUT_PET);
    // 当前布局不在掩码内时从头找第一个可用项
    assert(app_config_next_layout(APP_LAYOUT_PET, m, +1) == APP_LAYOUT_CARD);
    // 只有自身可用时保持不动
    assert(app_config_next_layout(APP_LAYOUT_QR, (uint8_t)(1 << APP_LAYOUT_QR), +1)
           == APP_LAYOUT_QR);
}

int main(void)
{
    test_defaults();
    test_sanitize();
    test_steps();
    test_layout_mask();
    return 0;
}
