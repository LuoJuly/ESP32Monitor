/**
 * @file lv_conf.h
 * LVGL Configuration for PC Monitor
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C (466x466 CO5300)
 */

#if 1 /* Set to 1 to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ============================================================================
 * COLOR SETTINGS
 * ========================================================================== */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* ============================================================================
 * MEMORY SETTINGS - USING PSRAM FOR FRAME BUFFERS
 * ========================================================================== */
#define LV_MEM_CUSTOM 1
#define LV_MEM_SIZE (128 * 1024U)  /* 128 KB for LVGL internal memory */

/* Custom memory allocator to prefer PSRAM (defined in main_lvgl.c) */
#define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC   lv_custom_malloc
#define LV_MEM_CUSTOM_FREE    lv_custom_free
#define LV_MEM_CUSTOM_REALLOC lv_custom_realloc

/* Forward declarations for custom allocators */
void *lv_custom_malloc(size_t size);
void lv_custom_free(void *ptr);
void *lv_custom_realloc(void *ptr, size_t size);

/* ============================================================================
 * DISPLAY SETTINGS
 * ========================================================================== */
#define LV_DPI_DEF 266  /* 1.75 inch, 466 px */

#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0

/* ============================================================================
 * DRAWING & RENDERING
 * ========================================================================== */
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4

/* Enable anti-aliasing for smooth circles/arcs */
#define LV_DRAW_SW_SUPPORT_ARGB8888 0
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4

/* ============================================================================
 * FONT SETTINGS
 * ========================================================================== */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ============================================================================
 * WIDGET USAGE - Only enable what we need
 * ========================================================================== */
#define LV_USE_LABEL 1
#define LV_USE_ARC 1      /* For circular gauges (CPU, GPU) */
#define LV_USE_BAR 1      /* For RAM bar */
#define LV_USE_CHART 1    /* For network graph */
#define LV_USE_LINE 0
#define LV_USE_IMG 0
#define LV_USE_BUTTON 0
#define LV_USE_SWITCH 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 1
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_CALENDAR 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* ============================================================================
 * CHART WIDGET SETTINGS (for Network Display)
 * ========================================================================== */
#define LV_USE_CHART_LINE 1
#define LV_CHART_AXIS_TICK_LABEL_MAX_LEN 20

/* ============================================================================
 * LOGGING
 * ========================================================================== */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO
#define LV_LOG_PRINTF 1

/* ============================================================================
 * OPTIMIZATION
 * ========================================================================== */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* Reduce memory usage */
#define LV_MEM_BUF_MAX_NUM 16
#define LV_CACHE_DEF_SIZE 0

/* ============================================================================
 * THEME
 * ========================================================================== */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

/* ============================================================================
 * TICK INTERFACE
 * ========================================================================== */
/* The Waveshare BSP (esp_lv_adapter) owns the LVGL tick. */
#define LV_TICK_CUSTOM 0

/* ============================================================================
 * OPERATING SYSTEM
 * ========================================================================== */
#define LV_USE_OS LV_OS_FREERTOS

#endif /* LV_CONF_H */

#endif /* Enable content */
