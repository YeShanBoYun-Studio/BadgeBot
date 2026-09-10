<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

The badge pages ship two application fonts, `main/fonts/font_cjk_20.*` (user
content: name, organization, title) and `main/fonts/font_cjk_16.*` (UI chrome:
settings labels, key hints, status text):

- LVGL 1 bpp bitmap fonts at 20 px and 16 px. They cost about 240 KB and
  200 KB of the 3 MB application partition respectively; 4 bpp would cost
  roughly 850 KB for the 20 px size alone.
- Missing glyphs (the `LV_SYMBOL_*` icons) fall back to `lv_font_montserrat_20`
  / `lv_font_montserrat_16`, which the fonts embed via `--lv-fallback`.
- The charset covers printable ASCII, GB2312 level-1 Chinese, common CJK
  punctuation, and every non-ASCII character that appears in `main/` source
  strings — level-2 characters used by new UI text are picked up automatically
  by `tools/gen_font_symbols.py`.
- Source font: Noto Sans SC Regular (SIL OFL 1.1). The generated `.inc` files
  are data, do not edit by hand; each wrapper `.c` only includes its `.inc`.
- Regenerate after adding new UI text (run from the repo root in a POSIX
  shell; the symbol list exceeds the Windows `cmd.exe` command-line length):

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

## Images

Store reusable source images and generated display assets in `images/`.

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
