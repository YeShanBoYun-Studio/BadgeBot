// main/ui_text.h —— 界面静态文案的中英双语表。
// 只收录"界面框架"文字(菜单/设置/提示/占位说明);用户内容(姓名/公司等)不在此列,
// 始终按用户输入原样显示。语言选择存 app_config.lang(0 = 中文,1 = English)。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// X(枚举名, 中文, English);带 % 的条目是 printf 格式串,调用处直接作为格式使用。
#define UI_TEXT_LIST(X) \
    X(SEL,          "选择",                  "Sel") \
    X(OPEN,         "进入",                  "Open") \
    X(ADJ,          "调整",                  "Adj") \
    X(BACK,         "回退",                  "Back") \
    X(EXIT,         "返回",                  "Exit") \
    X(MENU,         "菜单",                  "Menu") \
    X(SLEEP,        "息屏",                  "Sleep") \
    X(VIEW,         "布局",                  "View") \
    X(TOGGLE,       "开关",                  "Toggle") \
    X(SEND,         "翻页",                  "Turn") \
    X(KEYMAP,       "键位",                  "Key map") \
    X(MODE,         "模式",                  "Mode") \
    X(TURN,         "翻段",                  "Segment") \
    X(SPEED,        "速度",                  "Speed") \
    X(PAUSE,        "暂停",                  "Pause") \
    X(MODE_LK,      "联动",                  "Linked") \
    X(MODE_AUTO,    "自动",                  "Auto") \
    X(PAUSED,       "已暂停",                "Paused") \
    X(NO_NOTES,     "未上传讲稿\n门户「提词稿」保存", "No notes\nsave via portal") \
    X(HID_WAIT,     "等待电脑配对…",         "Waiting for pairing…") \
    X(HID_CONN,     "已连接,配对中…",        "Connected, pairing…") \
    X(HID_READY,    "已就绪,可翻页",         "Ready - turn away") \
    X(HID_MAP_PG,   "键位:PgUp / PgDn",     "Keys: PgUp / PgDn") \
    X(HID_MAP_AR,   "键位:左键 / 右键",     "Keys: Left / Right") \
    X(DONE,         "完成",                  "Done") \
    X(ROW_BL,       "亮度",                  "Brightness") \
    X(ROW_VOL,      "音量",                  "Volume") \
    X(ROW_LAYOUT,   "主页布局",              "Home layout") \
    X(ROW_LANG,     "语言",                  "Language") \
    X(ROW_THEME,    "主题",                  "Theme") \
    X(ROW_OFF,      "自动息屏",              "Auto off") \
    X(ROW_BOOT,     "开机页面",              "Boot page") \
    X(ROW_RESET,    "恢复出厂",              "Factory reset") \
    X(RESET_ARM,    "再按OK确认",            "Press OK again") \
    X(LAY_CARD,     "名片",                  "Card") \
    X(LAY_QR,       "二维码",                "QR codes") \
    X(LAY_PET,      "宠物",                  "Pet") \
    X(ON,           "开",                    "On") \
    X(OFF,          "关",                    "Off") \
    X(TH_LIGHT,     "浅色",                  "Light") \
    X(TH_DARK,      "深色",                  "Dark") \
    X(OFF_ALWAYS,   "常亮",                  "Always on") \
    X(OFF_MIN,      "%d 分钟",               "%d min") \
    X(BOOT_BADGE,   "主页",                  "Badge") \
    X(BOOT_MENU,    "菜单",                  "Menu") \
    X(LANG_ZH,      "中文",                  "中文") \
    X(LANG_EN,      "English",               "English") \
    X(NO_TIME,      "未校时",                "No time") \
    X(QR_UNSET,     "未配置",                "Not set") \
    X(ST_HUNGER,    "饱食",                  "Food") \
    X(ST_FUN,       "心情",                  "Fun") \
    X(ST_CLEAN,     "清洁",                  "Clean") \
    X(ST_ENERGY,    "精力",                  "Energy") \
    X(PET_STAGE_EGG,  "蛋",                  "Egg") \
    X(PET_STAGE_BABY, "幼年",                "Baby") \
    X(PET_STAGE_CHILD, "少年",               "Child") \
    X(PET_STAGE_ADULT, "成年",               "Adult") \
    X(PET_MOOD_HAPPY,  "开心",               "Happy") \
    X(PET_MOOD_FINE,   "还不错",             "Okay") \
    X(PET_MOOD_SAD,    "难过",               "Sad") \
    X(PET_MOOD_HUNGRY, "饿了",               "Hungry") \
    X(PET_MOOD_SLEEPY, "困了",               "Sleepy") \
    X(PET_MOOD_ASLEEP, "睡着了",             "Asleep") \
    X(PET_MOOD_NA,     "…",                  "…") \
    X(PET_HATCH,     "孵化中…",              "Hatching…") \
    X(PET_AGE_FMT,   "%d天%d时",             "%dd %dh") \
    X(PET_WT_FMT,    "体重 %dg",             "Weight %dg") \
    X(ACT_FEED,      "喂食",                 "Feed") \
    X(ACT_PLAY,      "玩耍",                 "Play") \
    X(ACT_CLEAN,     "清洁",                 "Clean") \
    X(ACT_SLEEP,     "睡觉",                 "Sleep") \
    X(ACT_WAKE,      "叫醒",                 "Wake") \
    X(PORTAL_STARTING, "正在启动热点…",      "Starting hotspot…") \
    X(PORTAL_DONE,  "配网已结束,按 OK 返回", "Portal closed - press OK") \
    X(PORTAL_URL,   "手机浏览器打开 %s",     "Open %s in a browser")

typedef enum {
#define X(id, zh, en) UI_T_##id,
    UI_TEXT_LIST(X)
#undef X
    UI_T_COUNT,
} ui_str_id;

// 按当前配置的语言取文案(目标端);在主机测试等无 app_config 环境下等同 lang=0。
const char *ui_text(ui_str_id id);
// 显式指定语言的纯查表(0 = 中文,1 = English);越界语言按中文处理。可被主机测试直接编译。
const char *ui_text_lang(unsigned lang, ui_str_id id);
// 星期几文案(0 = 周日/Sun .. 6),同样跟随语言。
const char *ui_text_weekday(unsigned lang, int wd);

#ifdef __cplusplus
}
#endif
