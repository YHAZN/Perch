// LVGL 9 configuration for Perch (240x284 RGB565, Arduino-ESP32, single UI thread).
// Anything not set here uses LVGL's defaults from lv_conf_internal.h.
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// Use the C library allocator: with CONFIG_SPIRAM_USE_MALLOC, large blocks land in PSRAM.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 16
// Wrap only at spaces and closing marks: "-4" and "3.14" must not split across lines.
#define LV_TXT_BREAK_CHARS " ,;:_)]}"
#define LV_DPI_DEF 160

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

// Three type sizes (12/16/24) plus the face clock (48).
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0

#endif
#endif
