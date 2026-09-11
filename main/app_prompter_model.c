// main/app_prompter_model.c —— 分段查表实现;见头文件。
#include "app_prompter_model.h"

// 取 text 第 line 行(0 基)的 [begin, end) 字节范围;跳过空行不计。
// 返回 false 表示行号超出非空行数。
static bool line_span(const char *text, int want, const char **begin, const char **end)
{
    const char *p = text;
    int nonempty = 0;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        const char *last = nl;
        if (last > p && last[-1] == '\r') last--;      // CRLF 容忍
        if (last != p) {                                // 非空行才计数
            if (nonempty == want) {
                *begin = p;
                *end = last;
                return true;
            }
            nonempty++;
        }
        if (!*nl) break;
        p = nl + 1;
    }
    return false;
}

int prompter_segment_count(const char *text)
{
    if (!text) return 0;
    int n = 0;
    const char *b, *e;
    while (n < PROMPTER_MAX_SEGMENTS && line_span(text, n, &b, &e)) n++;
    return n;
}

size_t prompter_max_line_len(const char *text)
{
    if (!text) return 0;
    size_t max_len = 0;
    const char *p = text;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        const char *last = nl;
        if (last > p && last[-1] == '\r') last--;
        if ((size_t)(last - p) > max_len) max_len = (size_t)(last - p);
        if (!*nl) break;
        p = nl + 1;
    }
    return max_len > 4096 ? 4096 : max_len;
}

bool prompter_segment(const char *text, int idx, char *out, size_t cap)
{
    if (!text || !out || cap == 0 || idx < 0) return false;
    const char *b, *e;
    if (!line_span(text, idx, &b, &e)) return false;

    size_t span = (size_t)(e - b);
    size_t len = span > cap - 1 ? cap - 1 : span;
    for (size_t i = 0; i < len; i++) out[i] = b[i];
    if (len < span) {
        // 截断:从前往后按 UTF-8 前导字节计字符宽,停在最后一个完整字符之后
        size_t i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)out[i];
            size_t w = (c & 0x80) == 0   ? 1
                       : (c & 0xE0) == 0xC0 ? 2
                       : (c & 0xF0) == 0xE0 ? 3
                                            : 4;
            if (i + w > len) break;
            i += w;
        }
        len = i;
    }
    out[len] = '\0';
    return true;
}
