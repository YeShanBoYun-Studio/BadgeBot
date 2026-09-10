<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

工牌页面自带两个应用字体：`main/fonts/font_cjk_20.*`（用户内容：姓名、公司、岗位）
与 `main/fonts/font_cjk_16.*`（界面框架：设置项、按键提示、状态文字）：

- LVGL 1 bpp 位图字体，20 px 与 16 px。分别占用 3 MB 应用分区的约 240 KB 与
  200 KB；若 20 px 用 4 bpp 单个就约需 850 KB。
- 缺失字形（`LV_SYMBOL_*` 图标）回退到 `lv_font_montserrat_20` /
  `lv_font_montserrat_16`，由生成时的 `--lv-fallback` 参数写入。
- 字符集覆盖可打印 ASCII、GB2312 一级汉字、常用中文标点，以及 `main/`
  源码字符串里出现过的全部非 ASCII 字符——新增文案用到的二级字（如“浏”）
  由 `tools/gen_font_symbols.py` 自动收进符号表。
- 源字体：Noto Sans SC Regular（SIL OFL 1.1）。生成的 `.inc` 是数据文件，勿手改；
  每个包装 `.c` 只负责 include 对应的 `.inc`。
- 新增界面文案后在仓库根目录用 `lv_font_conv` 1.5.3 重新生成（请在 POSIX
  shell 下执行；符号表超过 Windows `cmd.exe` 命令行长度上限）：

  ```sh
  npm install lv_font_conv@1.5.3
  python tools/gen_font_symbols.py /tmp/symbols.txt
  for sz in 20 16; do
    npx lv_font_conv --font NotoSansSC-Regular.otf --range 0x20-0x7E \
        --symbols "$(cat /tmp/symbols.txt)" \
        --size $sz --bpp 1 --format lvgl --lv-font-name font_cjk_$sz \
        --lv-include lvgl.h --lv-fallback lv_font_montserrat_$sz \
        --no-compress --output main/fonts/font_cjk_$sz.inc
  done
  ```

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
