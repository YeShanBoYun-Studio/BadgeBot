#!/usr/bin/env python3
"""门户页面压缩管道。

source of truth 是 main/portal.html(可读、可编辑);
本脚本把它 gzip 压缩成 main/portal_html_gz.h(C 字节数组),
app_portal.c 直接发送压缩字节,浏览器自动解压。
页面 25KB→约 7KB,解决 softAP 链路上大页面发送被截断、
页面"看得见但按钮全死"(尾部 <script> 丢失)的问题。

用法:
    python tools/gen_portal_gzip.py          # 重新生成 portal_html_gz.h
    python tools/gen_portal_gzip.py --check  # 校验头文件是否过期(用于 validate.sh)

首次迁移:若 main/portal.html 不存在,会从 app_portal.c 的 PORTAL_HTML
C 字符串里提取一次并删除该字符串(改为 #include 头文件)。
"""
import gzip
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT / "main" / "portal.html"
HEADER = ROOT / "main" / "portal_html_gz.h"
PORTAL_C = ROOT / "main" / "app_portal.c"

UNESCAPE = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}


def extract_c_string_literals(src: str, start: int):
    """解析从 start(指向第一个引号)开始的相邻 C 字符串字面量,直到 ';'。"""
    out = []
    i = start
    while True:
        while i < len(src) and src[i] in " \t\r\n":
            i += 1
        if i >= len(src):
            raise ValueError("PORTAL_HTML 字面量没有终止分号")
        if src[i] == ";":
            return "".join(out), i + 1
        if src[i] != '"':
            raise ValueError(f"PORTAL_HTML 解析意外字符: {src[i]!r}")
        i += 1
        while src[i] != '"':
            c = src[i]
            if c == "\\":
                n = src[i + 1]
                out.append(UNESCAPE.get(n, "\\" + n))
                i += 2
            else:
                out.append(c)
                i += 1
        i += 1  # 收尾引号


def migrate_from_c() -> None:
    """一次性迁移:从 app_portal.c 提取 PORTAL_HTML 并替换为 include。"""
    src = PORTAL_C.read_text(encoding="utf-8")
    marker = "static const char PORTAL_HTML[] ="
    pos = src.find(marker)
    if pos < 0:
        return
    quote = src.find('"', pos)
    html, end = extract_c_string_literals(src, quote)
    HTML.write_text(html, encoding="utf-8", newline="\n")
    line_start = src.rfind("\n", 0, pos) + 1
    src = src[:line_start] + '#include "portal_html_gz.h"\n' + src[end:]
    PORTAL_C.write_text(src, encoding="utf-8", newline="\n")
    print("[gen] 已从 app_portal.c 迁移 portal.html 并替换为 include")


def main() -> int:
    check = "--check" in sys.argv
    if not HTML.exists():
        migrate_from_c()
    html = HTML.read_text(encoding="utf-8")
    # mtime=0:去掉 gzip 头里的时间戳,保证输出字节确定(--check 才能比对)
    gz = gzip.compress(html.encode("utf-8"), 9, mtime=0)

    nums = [str(b) for b in gz]
    lines = ["  " + ",".join(nums[i:i + 16]) + "," for i in range(0, len(nums), 16)]
    header = (
        "// main/portal_html_gz.h —— 由 tools/gen_portal_gzip.py 生成,勿手改。\n"
        "// 源文件是 main/portal.html;改完页面后运行:\n"
        "//   python tools/gen_portal_gzip.py\n"
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        f"#define PORTAL_HTML_GZ_LEN {len(gz)}u\n"
        "static const uint8_t PORTAL_HTML_GZ[] = {\n" + "\n".join(lines) + "\n};\n"
    )

    if check:
        old = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        if old != header:
            print("portal_html_gz.h 过期:请运行 python tools/gen_portal_gzip.py",
                  file=sys.stderr)
            return 1
        return 0

    HEADER.write_text(header, encoding="utf-8", newline="\n")
    print(f"[gen] portal.html {len(html.encode('utf-8'))}B -> gzip {len(gz)}B "
          f"-> {HEADER.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
