// main/app_voice_model.c —— 语音应答解析与地址拼接的纯逻辑实现。
#include "app_voice_model.h"

#include <stdio.h>
#include <string.h>

// 在 JSON 里定位 "text" 键对应字符串值的起点;找不到返回 NULL。
static const char *find_text_value(const char *json)
{
    const char *k = strstr(json, "\"text\"");
    if (!k) return NULL;
    k += 6;
    while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    if (*k != ':') return NULL;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    return *k == '"' ? k + 1 : NULL;
}

// 单个多字节 UTF-8 序列的长度;非法首字节按 1 处理(显示层容忍)。
static int utf8_len(uint8_t lead)
{
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// 把解码出的一个码点按 UTF-8 写入 out(带 NUL 余量);空间不足按字符边界截断。
// 返回写入字节数;*full 置 1 表示缓冲已满。
static size_t emit_utf8(char *out, size_t cap, size_t used, uint32_t cp, bool *full)
{
    char buf[4];
    size_t n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    if (used + n + 1 > cap) {           // +1 留 NUL
        *full = true;
        return 0;
    }
    memcpy(out + used, buf, n);
    return n;
}

bool voice_result_text(const char *json, char *out, size_t cap)
{
    if (!json || !out || cap < 8) return false;
    const char *p = find_text_value(json);
    if (!p) return false;

    size_t used = 0;
    bool full = false;
    while (*p && *p != '"' && !full) {
        char c = *p;
        if (c != '\\') {                                  // 普通字符(含多字节原样收)
            int n = utf8_len((uint8_t)c);
            if (used + (size_t)n + 1 > cap) { full = true; break; }
            memcpy(out + used, p, (size_t)n);
            used += (size_t)n;
            p += n;
            continue;
        }
        p++;
        char e = *p++;
        switch (e) {
        case '"': case '\\': case '/':
            if (used + 2 > cap) { full = true; break; }
            out[used++] = e;
            break;
        case 'b': c = '\b'; goto plain;
        case 'f': c = '\f'; goto plain;
        case 'n': c = '\n'; goto plain;
        case 'r': c = '\r'; goto plain;
        case 't': c = '\t'; goto plain;
        plain:
            if (used + 2 > cap) { full = true; break; }
            out[used++] = c;
            break;
        case 'u': {
            uint32_t cp = 0;
            for (int i = 0; i < 4; i++) {
                char h = p[i];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                else return false;                        // 非法转义,宁可失败
            }
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00) {            // 高代理:取配对低半或丢弃
                cp = 0;
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                cp = 0;                                   // 孤立低代理:丢弃
            }
            if (cp != 0) used += emit_utf8(out, cap, used, cp, &full);
            break;
        }
        default:
            return false;
        }
    }
    if (*p && *p != '"') {                                // 值没正常收尾
        if (!full) return false;
    }
    out[used] = '\0';
    return true;
}

bool voice_url_valid(const char *url)
{
    if (!url) return false;
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *host = url + 7;
    if ((uint8_t)*host <= ' ') return false;
    for (const char *s = host; *s; s++) {
        if (*s == '/') break;
        if ((uint8_t)*s <= ' ') return false;             // 主机名里不允许空白
    }
    return true;
}

void voice_host_display(const char *url, char *out, size_t cap)
{
    if (!out || cap < 4) return;
    out[0] = '\0';
    if (!voice_url_valid(url)) return;
    const char *host = url + 7;
    size_t n = 0;
    while (host[n] && host[n] != '/' && n + 1 < cap) {
        out[n] = host[n];
        n++;
    }
    out[n] = '\0';
}

bool voice_endpoint(const char *url, int rate, int bits, int ch, char *out, size_t cap)
{
    if (!voice_url_valid(url) || !out) return false;
    // "/voice" 属于路径,必须插在已有 query 之前
    const char *q = strchr(url, '?');
    int n;
    if (q) {
        n = snprintf(out, cap, "%.*s/voice?%s&rate=%d&bits=%d&ch=%d",
                     (int)(q - url), url, q + 1, rate, bits, ch);
    } else {
        n = snprintf(out, cap, "%s/voice?rate=%d&bits=%d&ch=%d", url, rate, bits, ch);
    }
    return n > 0 && (size_t)n < cap;
}
