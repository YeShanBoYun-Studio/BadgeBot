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
    assert(!c.hide_org_title);
    assert(strlen(c.name) > 0);
    assert(c.org[0] == '\0' && c.title[0] == '\0');
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
    assert(strcmp(app_config_layout_name(APP_LAYOUT_GITHUB), "GITHUB") == 0);
    assert(strcmp(app_config_layout_name(APP_LAYOUT_COUNT), "?") == 0);
}

int main(void)
{
    test_defaults();
    test_sanitize();
    test_steps();
    return 0;
}
