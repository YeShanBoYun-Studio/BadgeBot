// tests/test_app_voice.c —— 语音应答解析与地址拼接的主机测试。
// 只编译 main/app_voice_model.c,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>
#include "app_voice_model.h"

// 与被测实现一致的序列长度判定(测试自身的校验器用)。
static int utf8_len(unsigned char lead)
{
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

static void test_result_text(void)
{
    char out[VOICE_TEXT_CAP];

    // 基本字段 + 前后缀内容
    assert(voice_result_text("{\"ms\":123,\"text\":\"你好世界\"}", out, sizeof(out)));
    assert(strcmp(out, "你好世界") == 0);

    // 没有字段 / 结构不对
    assert(!voice_result_text("{\"ms\":123}", out, sizeof(out)));
    assert(!voice_result_text("not json", out, sizeof(out)));
    assert(!voice_result_text("{\"text\":123}", out, sizeof(out)));
    assert(!voice_result_text(NULL, out, sizeof(out)));

    // 转义:换行、引号、反斜杠、斜杠、制表
    assert(voice_result_text("{\"text\":\"a\\nb\\\"c\\\\d\\/e\\tf\"}", out, sizeof(out)));
    assert(strcmp(out, "a\nb\"c\\d/e\tf") == 0);

    // \uXXXX:中文与 ASCII
    assert(voice_result_text("{\"text\":\"\\u4f60\\u597d ok\"}", out, sizeof(out)));
    assert(strcmp(out, "你好 ok") == 0);

    // 代理对:取不到完整配对时丢弃,不产生坏字节
    assert(voice_result_text("{\"text\":\"\\ud83d\\ude00x\"}", out, sizeof(out)));
    assert(strcmp(out, "x") == 0);

    // UTF-8 原样字节直通
    assert(voice_result_text("{\"text\":\"直接中文\"}", out, sizeof(out)));
    assert(strcmp(out, "直接中文") == 0);

    // 截断按 UTF-8 边界:小缓冲装下前几个字符即可,整串必须能完整解码
    char small[8];
    assert(voice_result_text("{\"text\":\"abcd中文\"}", small, sizeof(small)));
    size_t n = strlen(small);
    assert(n > 0 && n <= 7);
    for (size_t i = 0; i < n; i += (size_t)utf8_len((unsigned char)small[i])) {
        assert(i + (size_t)utf8_len((unsigned char)small[i]) <= n);  // 序列不残缺
    }

    // 空文本是合法结果
    assert(voice_result_text("{\"text\":\"\"}", out, sizeof(out)));
    assert(out[0] == '\0');
}

static void test_url(void)
{
    assert(voice_url_valid("http://192.168.1.5:8722"));
    assert(voice_url_valid("http://pc.local:8080/sub"));
    assert(!voice_url_valid(NULL));
    assert(!voice_url_valid(""));
    assert(!voice_url_valid("https://api.example.com"));   // 明文通道 only
    assert(!voice_url_valid("http://"));
    assert(!voice_url_valid("ftp://x"));
    assert(!voice_url_valid("http://ho st:80"));
}

static void test_host_display(void)
{
    char out[32];
    voice_host_display("http://192.168.1.5:8722/base", out, sizeof(out));
    assert(strcmp(out, "192.168.1.5:8722") == 0);
    voice_host_display("https://x", out, sizeof(out));
    assert(out[0] == '\0');
    voice_host_display("http://abc", out, 4);              // 截断仍 NUL 结尾
    assert(strlen(out) == 3);
}

static void test_endpoint(void)
{
    char out[160];
    assert(voice_endpoint("http://192.168.1.5:8722", 16000, 16, 1, out, sizeof(out)));
    assert(strcmp(out, "http://192.168.1.5:8722/voice?rate=16000&bits=16&ch=1") == 0);
    assert(voice_endpoint("http://h/vbase?k=1", 8000, 16, 1, out, sizeof(out)));
    assert(strcmp(out, "http://h/vbase/voice?k=1&rate=8000&bits=16&ch=1") == 0);
    assert(!voice_endpoint("https://h", 16000, 16, 1, out, sizeof(out)));
    char tiny[20];
    assert(!voice_endpoint("http://192.168.1.5:8722", 16000, 16, 1, tiny, sizeof(tiny)));
}

int main(void)
{
    test_result_text();
    test_url();
    test_host_display();
    test_endpoint();
    return 0;
}
