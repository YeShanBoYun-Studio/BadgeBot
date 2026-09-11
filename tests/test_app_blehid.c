// tests/test_app_blehid.c —— 翻页器键位映射纯逻辑的主机测试。
// 只编译 main/app_blehid_keys.c,不依赖 ESP-IDF。
#include <assert.h>
#include "app_blehid_keys.h"

static void test_page_map(void)
{
    // 默认(0):演示文稿/PDF 键位
    assert(app_blehid_keycode(APP_BLEHID_MAP_PAGE, -1) == 0x4B);  // PageUp
    assert(app_blehid_keycode(APP_BLEHID_MAP_PAGE, +1) == 0x4E);  // PageDown
    // 其余未知模式回退为翻页键(与 sanitize 的越界清零一致)
    assert(app_blehid_keycode(9, +1) == 0x4E);
}

static void test_arrow_map(void)
{
    // 方向键(1):图片查看器/漫画键位
    assert(app_blehid_keycode(APP_BLEHID_MAP_ARROWS, -1) == 0x50); // Left
    assert(app_blehid_keycode(APP_BLEHID_MAP_ARROWS, +1) == 0x4F); // Right
}

int main(void)
{
    test_page_map();
    test_arrow_map();
    return 0;
}
