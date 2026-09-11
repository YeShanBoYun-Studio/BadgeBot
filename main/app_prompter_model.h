// main/app_prompter_model.h —— 提词稿分段的纯逻辑(不依赖 ESP-IDF,主机可测)。
// 分段约定(用户拍板):一行 = 一段(对应 PPT 一页);空行跳过;\r 容忍。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROMPTER_MAX_SEGMENTS 999   // 段数上限(三位数进度显示的容量)

// 非空行数,封顶 PROMPTER_MAX_SEGMENTS;text 为 NULL 时返回 0。
int prompter_segment_count(const char *text);

// 最长非空行的字节长(不含换行与 NUL);为分配"当前段"显示缓冲用。
// 上限 4096,超过按 4096 计。
size_t prompter_max_line_len(const char *text);

// 把第 idx 段(0 基)拷进 out(总是 NUL 结尾;过长时按 UTF-8 字符边界截断)。
// 越界或 text 为 NULL 返回 false。
bool prompter_segment(const char *text, int idx, char *out, size_t cap);
