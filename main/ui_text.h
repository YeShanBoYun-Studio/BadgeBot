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
    X(DONE,         "完成",                  "Done") \
    X(ROW_BL,       "亮度",                  "Brightness") \
    X(ROW_VOL,      "音量",                  "Volume") \
    X(ROW_LAYOUT,   "主页布局",              "Home layout") \
    X(ROW_LANG,     "语言",                  "Language") \
    X(ROW_THEME,    "主题",                  "Theme") \
    X(ROW_OFF,      "自动息屏",              "Auto off") \
    X(ROW_BOOT,     "开机页面",              "Boot page") \
    X(ROW_ABOUT,    "固件版本",              "Firmware") \
    X(LAY_CARD,     "名片",                  "Card") \
    X(LAY_QR,       "二维码",                "QR codes") \
    X(LAY_GH,       "GitHub",                "GitHub") \
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
    X(QR_LINK,      "链接",                  "Link") \
    X(QR_IMG,       "图片",                  "Photo") \
    X(QR_NONE,      "未上传\n二维码图",      "No QR\nimage") \
    X(GH_TITLE,     "GitHub 热力图",         "Contributions") \
    X(GH_WAIT,      "等待网络连接…",         "Waiting for Wi-Fi…") \
    X(GH_WAIT_SUB,  "联网后自动拉取提交记录", "Fetches once online") \
    X(GH_NOUSER,    "未配置用户名",          "No username set") \
    X(GH_NOUSER_SUB, "请在门户“GitHub 热力图”中填写", "Set it in the portal") \
    X(GH_TOTAL,     "近一年 %d 次提交",      "%d commits this year") \
    X(LOWMEM,       "内存不足",              "Low memory") \
    X(PET_ENERGY,   "活力 %d%%",             "Energy %d%%") \
    X(PET_MOOD,     "心情:开心",             "Mood: happy") \
    X(PET_SOON,     "养成系统制作中…",       "Raising system coming soon…") \
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
