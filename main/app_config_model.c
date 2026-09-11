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
    snprintf(cfg->qr_text[0], sizeof(cfg->qr_text[0]), "%s",
             "https://github.com/YeShanBoYun-Studio/BadgeBot");
    cfg->hide_org_title = true;   // 公司/岗位默认隐藏,门户里可改
    cfg->layout = APP_LAYOUT_CARD;
    cfg->layout_mask = APP_LAYOUT_ALL_MASK;
    cfg->lang = 0;                // 中文
    cfg->theme = 0;               // 浅色
    cfg->brightness = APP_CFG_BL_MAX;
    cfg->volume = 60;
    cfg->screen_off_min = 5;
    cfg->boot_badge = true;
    cfg->hid_arrows = 0;          // 演示文稿场景最常见,默认 PgUp/PgDn
}

// 字符串字段只保证 NUL 结尾;截断产生的残缺 UTF-8 尾字节由显示层容忍(LVGL 跳过非法序列)。
static bool terminate(char *s, size_t cap) {
    if (memchr(s, '\0', cap)) return false;
    s[cap - 1] = '\0';
    return true;
}

static bool clamp_u8(uint8_t *v, uint8_t lo, uint8_t hi) {
    if (*v < lo) { *v = lo; return true; }
    if (*v > hi) { *v = hi; return true; }
    return false;
}

bool app_config_sanitize(app_config_t *cfg) {
    bool changed = false;
    changed |= terminate(cfg->name, sizeof(cfg->name));
    changed |= terminate(cfg->org, sizeof(cfg->org));
    changed |= terminate(cfg->title, sizeof(cfg->title));
    changed |= terminate(cfg->voice_url, sizeof(cfg->voice_url));
    for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
        changed |= terminate(cfg->qr_text[i], sizeof(cfg->qr_text[i]));
        changed |= terminate(cfg->qr_label[i], sizeof(cfg->qr_label[i]));
    }
    if (cfg->qr_mode > 0x0F) { cfg->qr_mode = 0; changed = true; }
    if (cfg->hid_arrows > 1) { cfg->hid_arrows = 0; changed = true; }
    if (cfg->layout >= APP_LAYOUT_COUNT) { cfg->layout = APP_LAYOUT_CARD; changed = true; }
    if (cfg->lang > 1) { cfg->lang = 0; changed = true; }
    if (cfg->layout_mask == 0 || cfg->layout_mask > APP_LAYOUT_ALL_MASK) {
        cfg->layout_mask = APP_LAYOUT_ALL_MASK;
        changed = true;
    }
    // 默认布局被关掉时,挪到第一个开启的布局,避免开机进入一个被禁用的页面
    if (!(cfg->layout_mask & (1 << cfg->layout))) {
        cfg->layout = app_config_next_layout(cfg->layout, cfg->layout_mask, +1);
        changed = true;
    }
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

uint8_t app_config_next_layout(uint8_t cur, uint8_t mask, int dir) {
    if (cur >= APP_LAYOUT_COUNT) cur = APP_LAYOUT_CARD;
    if (!(mask & (1 << cur))) {           // 当前布局被禁用:回第一个可用项
        for (uint8_t i = 0; i < APP_LAYOUT_COUNT; i++) {
            if (mask & (1 << i)) return i;
        }
        return cur;                       // 掩码为空(上游应已兜底为全开)
    }
    for (int i = 1; i <= APP_LAYOUT_COUNT; i++) {
        uint8_t cand = (uint8_t)((cur + APP_LAYOUT_COUNT + (dir >= 0 ? i : APP_LAYOUT_COUNT - i))
                                 % APP_LAYOUT_COUNT);
        if (mask & (1 << cand)) return cand;
    }
    return cur;                           // 只有自身可用
}

const char *app_config_layout_name(uint8_t layout) {
    switch (layout) {
    case APP_LAYOUT_CARD:   return "CARD";
    case APP_LAYOUT_QR:     return "QR";
    case APP_LAYOUT_PET:    return "PET";
    default:                return "?";
    }
}
