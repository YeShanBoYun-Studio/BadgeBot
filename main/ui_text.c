// main/ui_text.c —— 双语文案表;见 ui_text.h 的 UI_TEXT_LIST。
#include "ui_text.h"
#include "app_config.h"

#define LANG_MAX 2

static const char *const TABLE[UI_T_COUNT][LANG_MAX] = {
#define X(id, zh, en) [UI_T_##id] = { zh, en },
    UI_TEXT_LIST(X)
#undef X
};

static const char *const WEEKDAY[7][2] = {
    { "周日", "Sun" }, { "周一", "Mon" }, { "周二", "Tue" }, { "周三", "Wed" },
    { "周四", "Thu" }, { "周五", "Fri" }, { "周六", "Sat" },
};

const char *ui_text_lang(unsigned lang, ui_str_id id)
{
    if ((int)id >= UI_T_COUNT) return "?";
    if (lang >= LANG_MAX) lang = 0;
    return TABLE[id][lang];
}

const char *ui_text_weekday(unsigned lang, int wd)
{
    if (wd < 0 || wd > 6) wd = 0;
    if (lang >= LANG_MAX) lang = 0;
    return WEEKDAY[wd][lang];
}

const char *ui_text(ui_str_id id)
{
#ifdef ESP_PLATFORM
    return ui_text_lang(app_config_get()->lang, id);
#else
    return ui_text_lang(0, id);
#endif
}
