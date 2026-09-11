// main/app_voice.h —— V2 语音:徽章录音,经局域网明文 HTTP 直传电脑后端,
// 由后端识别并注入电脑键盘;徽章只当"无线麦克风 + 按键",全程只用 Wi-Fi
// (蓝牙/Wi-Fi 射频互斥,且设备端 TLS 验签有已知问题,见 CHANGELOG)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VOICE_IDLE = 0,     // 就绪,等开始
    VOICE_WAIT_NET,     // 正在按需联网
    VOICE_REC,          // 录音中(OK 停止并发送)
    VOICE_SEND,         // 上传与等待识别
    VOICE_DONE,         // 完成:识别文本可取
    VOICE_ERR,          // 失败:app_voice_error() 给出原因
} voice_state_t;

typedef enum {
    VOICE_OK = 0,
    VOICE_ERR_NOURL,    // 门户里还没配语音后端地址
    VOICE_ERR_NET,      // Wi-Fi 连不上
    VOICE_ERR_CONN,     // 连不上后端服务器
    VOICE_ERR_SEND,     // 录音传输中断
    VOICE_ERR_HTTP,     // 后端返回非 200
    VOICE_ERR_EMPTY,    // 后端没有返回文本
    VOICE_ERR_MIC,      // 麦克风初始化/读取失败
} voice_err_t;

// 请求开始一轮"联网 → 录音 → 识别";忙时忽略。worker 任务在首次调用时创建。
void app_voice_start(void);
// 录音期间请求:立即停止录音并把已录内容送出;其余状态无效。
void app_voice_stop(void);

voice_state_t app_voice_state(void);
voice_err_t app_voice_error(void);
// 录音已进行的毫秒数(REC 状态下页面计时用)。
uint32_t app_voice_elapsed_ms(void);
// 取走最近一轮的识别文本(DONE 状态下有效,取后清空)。
bool app_voice_take_text(char *out, size_t cap);
