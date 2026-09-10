// main/app_config_model.c —— 配置的默认值、校验与设置页步进。
// 不包含任何 ESP-IDF 头文件,tests/test_app_config.c 在主机上直接编译本文件。
#include "app_config.h"
#include <stdio.h>
#include <string.h>

// 自动息屏可选档位(分钟),0 = 常亮。设置页只在这些值之间循环。
static const uint8_t OFF_STEPS[] = { 0, 1, 5, 10, 30 };
#define OFF_STEP_COUNT (sizeof(OFF_STEPS) / sizeof(OFF_STEPS[0]))

void app_config_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "%s", "BadgeBot");
    snprintf(cfg->qr_a, sizeof(cfg->qr_a), "%s",
             "https://github.com/YeShanBoYun-Studio/BadgeBot");
    cfg->hide_org_title = true;   // 公司/岗位默认隐藏,门户里可改
    cfg->layout = APP_LAYOUT_CARD;
    cfg->theme = 0;               // 浅色
    cfg->brightness = APP_CFG_BL_MAX;
    cfg->volume = 60;
    cfg->screen_off_min = 5;
    cfg->boot_badge = true;
}

// 字符串字段只保证 NUL 结尾;截断产生的残缺 UTF-8 尾字节由显示层容忍(LVGL 跳过非法序列)。
static bool terminate(char *s) {
    if (memchr(s, '\0', APP_CFG_STR_LEN)) return false;
    s[APP_CFG_STR_LEN - 1] = '\0';
    return true;
}

static bool clamp_u8(uint8_t *v, uint8_t lo, uint8_t hi) {
    if (*v < lo) { *v = lo; return true; }
    if (*v > hi) { *v = hi; return true; }
    return false;
}

bool app_config_sanitize(app_config_t *cfg) {
    bool changed = false;
    changed |= terminate(cfg->name);
    changed |= terminate(cfg->org);
    changed |= terminate(cfg->title);
    changed |= terminate(cfg->qr_a);
    if (cfg->layout >= APP_LAYOUT_COUNT) { cfg->layout = APP_LAYOUT_CARD; changed = true; }
    if (cfg->theme > 1) { cfg->theme = 0; changed = true; }
    changed |= clamp_u8(&cfg->brightness, APP_CFG_BL_MIN, APP_CFG_BL_MAX);
    changed |= clamp_u8(&cfg->volume, 0, APP_CFG_VOL_MAX);
    changed |= clamp_u8(&cfg->screen_off_min, 0, APP_CFG_OFF_MAX);
    return changed;
}

// 在 [lo, hi] 的等距网格上步进并回绕;先把当前值吸附到网格,避免门户写入的任意值让步进错位。
static uint8_t step_grid(uint8_t cur, uint8_t lo, uint8_t hi, uint8_t step, int dir) {
    if (cur < lo) cur = lo;
    if (cur > hi) cur = hi;
    int snapped = lo + ((cur - lo + step / 2) / step) * step;
    int next = snapped + (dir >= 0 ? step : -step);
    if (next > hi) next = lo;
    if (next < lo) next = hi;
    return (uint8_t)next;
}

uint8_t app_config_step_brightness(uint8_t cur, int dir) {
    return step_grid(cur, APP_CFG_BL_MIN, APP_CFG_BL_MAX, APP_CFG_BL_STEP, dir);
}

uint8_t app_config_step_volume(uint8_t cur, int dir) {
    return step_grid(cur, 0, APP_CFG_VOL_MAX, APP_CFG_VOL_STEP, dir);
}

uint8_t app_config_step_screen_off(uint8_t cur, int dir) {
    // 取“不大于当前值的最大档”作为当前档,越界值也能落到确定的位置。
    int idx = 0;
    for (int i = 0; i < (int)OFF_STEP_COUNT; i++) {
        if (OFF_STEPS[i] <= cur) idx = i;
    }
    idx += (dir >= 0) ? 1 : -1;
    if (idx >= (int)OFF_STEP_COUNT) idx = 0;
    if (idx < 0) idx = OFF_STEP_COUNT - 1;
    return OFF_STEPS[idx];
}

const char *app_config_layout_name(uint8_t layout) {
    switch (layout) {
    case APP_LAYOUT_CARD:   return "CARD";
    case APP_LAYOUT_QR:     return "QR";
    case APP_LAYOUT_GITHUB: return "GITHUB";
    default:                return "?";
    }
}
