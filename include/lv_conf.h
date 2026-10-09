// LVGL 9 configuration for Perch (240x284 RGB565, Arduino-ESP32, single UI thread).
// Anything not set here uses LVGL's defaults from lv_conf_internal.h.
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// Use the C library allocator: with CONFIG_SPIRAM_USE_MALLOC, large blocks land in PSRAM.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM  // src/lv_psram.c: widgets live in PSRAM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
/* Cache each object's resolved style values (measured ~10% faster chat drawing). */
#define LV_OBJ_STYLE_CACHE 1
#define LV_DEF_REFR_PERIOD 16
// Wrap only at spaces and closing marks: "-4" and "3.14" must not split across lines.
#define LV_TXT_BREAK_CHARS " ,;:_)]}"
#define LV_DPI_DEF 160

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

// UI text uses the generated fonts in src/fonts; LVGL's own are only the default and the
// face clock.
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0

#endif
#endif
