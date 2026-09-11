// tests/test_prompter_model.c —— 提词稿分段纯逻辑的主机测试。
// 只编译 main/app_prompter_model.c,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>
#include "app_prompter_model.h"

static void test_count(void)
{
    assert(prompter_segment_count(NULL) == 0);
    assert(prompter_segment_count("") == 0);
    // 一行一段;空行跳过;CRLF 容忍
    assert(prompter_segment_count("第一页\n第二页\n第三页") == 3);
    assert(prompter_segment_count("第一页\r\n\r\n第二页\r\n") == 2);
    assert(prompter_segment_count("\n\n\n只有一段\n") == 1);
    assert(prompter_segment_count("single") == 1);
}

static void test_get(void)
{
    char buf[64];
    assert(prompter_segment("alpha\nbeta\ngamma", 1, buf, sizeof(buf)));
    assert(strcmp(buf, "beta") == 0);
    assert(prompter_segment("alpha\nbeta", 0, buf, sizeof(buf)));
    assert(strcmp(buf, "alpha") == 0);
    assert(prompter_segment("alpha\nbeta", 1, buf, sizeof(buf)));
    assert(strcmp(buf, "beta") == 0);
    assert(!prompter_segment("alpha", 1, buf, sizeof(buf)));   // 越界
    assert(!prompter_segment("alpha", -1, buf, sizeof(buf)));
    assert(!prompter_segment(NULL, 0, buf, sizeof(buf)));

    // \r 不得混入内容
    assert(prompter_segment("a\r\nb", 0, buf, sizeof(buf)));
    assert(strcmp(buf, "a") == 0);
}

static void test_truncate_utf8(void)
{
    // 每个汉字 3 字节:cap=8 放得下两个汉字(6 字节),第三个只装 2 字节 → 回退到边界
    char buf[8];
    const char *zh = "一二三四";
    assert(prompter_segment(zh, 0, buf, sizeof(buf)));
    assert(strcmp(buf, "一二") == 0);

    char tiny[4];                                              // 恰好一个汉字 + NUL
    assert(prompter_segment(zh, 0, tiny, sizeof(tiny)));
    assert(strcmp(tiny, "一") == 0);
}

static void test_max_line(void)
{
    assert(prompter_max_line_len(NULL) == 0);
    assert(prompter_max_line_len("") == 0);
    assert(prompter_max_line_len("ab\ncdef\n\ngh") == 4);
    assert(prompter_max_line_len("一二三\r\nok") == 9);       // UTF-8 按字节计
}

int main(void)
{
    test_count();
    test_get();
    test_truncate_utf8();
    test_max_line();
    return 0;
}
