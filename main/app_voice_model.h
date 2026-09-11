// main/app_voice_model.h —— 语音后端应答的纯逻辑解析与展示辅助。
// 不依赖 ESP-IDF,tests/test_app_voice.c 在主机上直接编译本文件。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 单次录音上限(秒):到点自动停并送出,避免误触发把麦克风一直开着。
#define VOICE_MAX_RECORD_SEC 30
// 识别结果缓冲上限(UTF-8 字节,含 NUL;约 120 个汉字)。
#define VOICE_TEXT_CAP 384

// 从后端应答(JSON)里提取 "text" 字段:处理 \\ \" \/ \b \f \n \r \t 与 \uXXXX
// (基本平面,代理对取低半并跳过)。找不到字段或结构不对返回 false;
// 缓冲不够时按 UTF-8 字符边界截断,仍然返回 true。
bool voice_result_text(const char *json, char *out, size_t cap);

// 语音后端地址是否可用:要求 http:// 前缀 + 主机名;本机走局域网明文通道,
// 不支持 https(设备端 TLS 验签有已知问题,见 CHANGELOG 热力图条目)。
bool voice_url_valid(const char *url);

// 取用于页面展示的主机:port 部分(去掉 scheme 与路径,超长按字节截断);
// out 必须非空且 cap >= 4,始终 NUL 结尾。
void voice_host_display(const char *url, char *out, size_t cap);

// 拼后端请求地址:url + "/voice?rate=&bits=&ch=";url 已带 query 时用 & 续接。
// url 非法或缓冲不足返回 false(out 内容未定义)。
bool voice_endpoint(const char *url, int rate, int bits, int ch, char *out, size_t cap);
