/**
 * @file screens_lvgl.h
 * @brief LVGL widgets for the four PC metrics
 *
 * On the Waveshare 1.75C they share one 466x466 display as four
 * full-screen pages (CPU, GPU, RAM, Network).
 * - CPU gauge (arc)
 * - GPU gauge (arc)
 * - RAM bar
 * - Network chart
 */

#ifndef SCREENS_LVGL_H
#define SCREENS_LVGL_H

#include "lvgl.h"
#include <stdint.h>
#include "core/system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Screen structures (exposed for theme access)
 */
#define NETWORK_HISTORY_SIZE 60

/* last_* fields cache the previously displayed values so update functions can
 * skip redundant LVGL calls. The display task runs at 10 FPS but real data
 * arrives ~1x/s; without this every widget is redrawn 10x/s (visible flicker,
 * and the network chart scrolls 10x too fast). Sentinel -999 forces the first
 * update to draw. */
#define SCREEN_VALUE_SENTINEL (-999)

struct screen_cpu_t {
    lv_obj_t *screen;
    lv_obj_t *arc;
    lv_obj_t *label_title;
    lv_obj_t *label_percent;
    lv_obj_t *label_temp;
    int16_t last_percent;
    float last_temp;
};

struct screen_gpu_t {
    lv_obj_t *screen;
    lv_obj_t *arc;
    lv_obj_t *label_title;
    lv_obj_t *label_percent;
    lv_obj_t *label_temp;
    lv_obj_t *label_vram;
    int16_t last_percent;
    float last_temp;
    float last_vram_used;
    float last_vram_total;
};

struct screen_ram_t {
    lv_obj_t *screen;
    lv_obj_t *label_title;
    lv_obj_t *label_value;
    lv_obj_t *label_percent;
    lv_obj_t *bar;
    lv_obj_t *label_total;
    float last_used;
    float last_total;
};

struct screen_network_t {
    lv_obj_t *screen;
    lv_obj_t *label_header;
    lv_obj_t *label_conn_type;
    lv_obj_t *label_speed;
    lv_obj_t *chart;
    lv_chart_series_t *ser_down;
    lv_chart_series_t *ser_up;
    lv_obj_t *label_down;
    lv_obj_t *label_up;
    int history_index;
    float last_down;
    float last_up;
    char last_type[16];
    char last_speed[16];
};

struct screen_weather_t {
    lv_obj_t *screen;
    lv_obj_t *icon;
    lv_obj_t *label_city;
    lv_obj_t *label_temp;
    lv_obj_t *label_hum;
    lv_obj_t *label_cond;
    int16_t last_temp;
    int16_t last_code;
    int16_t last_hum;
    char last_city[24];
};

struct screen_clock_t {
    lv_obj_t *screen;
    lv_obj_t *hour_label[12];
    lv_obj_t *moon_part[24];
    lv_obj_t *sun_part[48];
    uint8_t moon_n;
    uint8_t sun_n;
    int32_t last_sec;
    int16_t last_hour;
    int16_t last_minute;
    int16_t last_second;
    int8_t last_day;
    int8_t last_blink;
};

typedef struct screen_cpu_t screen_cpu_t;
typedef struct screen_gpu_t screen_gpu_t;
typedef struct screen_ram_t screen_ram_t;
typedef struct screen_network_t screen_network_t;
typedef struct screen_weather_t screen_weather_t;
typedef struct screen_clock_t screen_clock_t;

/**
 * @brief Black, borderless panel that fills a dashboard quadrant
 */
static inline lv_obj_t *screen_panel_create(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

/* Let a horizontal swipe reach the tileview instead of stopping on a gauge. */
static inline void screen_pass_swipe(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        screen_pass_swipe(lv_obj_get_child(obj, i));
    }
}

/* ============================================================================
 * SCREEN 1: CPU GAUGE (Ring with percentage and temperature)
 * ========================================================================== */
screen_cpu_t *screen_cpu_create(lv_obj_t *parent);
void screen_cpu_update(screen_cpu_t *screen, const pc_stats_t *stats);

/* ============================================================================
 * SCREEN 2: GPU GAUGE (Ring with percentage, temperature, VRAM)
 * ========================================================================== */
screen_gpu_t *screen_gpu_create(lv_obj_t *parent);
void screen_gpu_update(screen_gpu_t *screen, const pc_stats_t *stats);

/* ============================================================================
 * SCREEN 3: RAM BAR (Horizontal bar with segments)
 * ========================================================================== */
screen_ram_t *screen_ram_create(lv_obj_t *parent);
void screen_ram_update(screen_ram_t *screen, const pc_stats_t *stats);

/* ============================================================================
 * SCREEN 4: NETWORK GRAPH (Cyberpunk style with chart)
 * ========================================================================== */
screen_network_t *screen_network_create(lv_obj_t *parent);
void screen_network_update(screen_network_t *screen, const pc_stats_t *stats);

screen_weather_t *screen_weather_create(lv_obj_t *parent);
void screen_weather_update(screen_weather_t *screen, const pc_stats_t *stats);

screen_clock_t *screen_clock_create(lv_obj_t *parent);
void screen_clock_update(screen_clock_t *screen, const pc_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* SCREENS_LVGL_H */
