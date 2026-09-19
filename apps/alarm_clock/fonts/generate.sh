#!/bin/sh
# Regenerate the clock-face digit font. Needs Node (npx fetches lv_font_conv).
# Montserrat is licensed under the SIL Open Font License (OFL.txt).
set -e
cd "$(dirname "$0")"
npx --yes lv_font_conv@1.5.3 --font Montserrat-SemiBold.ttf --range 0x2D,0x30-0x3A \
    --size 120 --bpp 4 --no-compress --format lvgl --lv-include lvgl.h \
    --lv-font-name font_clock_120 -o ../main/font_clock_120.c
