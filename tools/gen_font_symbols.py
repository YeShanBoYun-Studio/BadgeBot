#!/usr/bin/env python3
# tools/gen_font_symbols.py —— 生成 CJK 字库的 --symbols 字符集文件。
#
# 字符集 = GB2312 一级汉字 + 常用中文标点 + main/ 源码字符串里出现过的全部
# 非 ASCII 字符(含 GB2312 二级字,如"浏""渲")。新增界面文案后重跑本脚本与
# lv_font_conv 即可补字,见 assets/README.md 的转换命令。
#
# 用法: python tools/gen_font_symbols.py <输出文件路径>
import glob
import io
import os
import sys

# 源码目录(相对仓库根);从本文件位置推导,便于在任何工作目录调用。
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PUNCT = "，。：；？！、（）《》〈〉“”‘’—…·「」『』【】％＋－～"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python tools/gen_font_symbols.py <output-file>")
        return 2

    chars = set(PUNCT)
    # GB2312 level-1: 0xB0A1..0xD7FE
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # 源码里实际出现的全部非 ASCII 字符
    for pat in ("main/*.c", "main/*.h", "main/fonts/*.h"):
        for f in glob.glob(os.path.join(ROOT, pat)):
            for ch in io.open(f, encoding="utf-8").read():
                if ord(ch) > 0x7E:
                    chars.add(ch)

    sym = "".join(sorted(chars))
    with io.open(sys.argv[1], "w", encoding="utf-8") as f:
        f.write(sym)
    print("symbols:", len(sym))
    return 0


if __name__ == "__main__":
    sys.exit(main())
