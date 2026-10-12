/**
 * @file screen_network_lvgl.c
 * @brief Network Graph Screen (Display 4) - LVGL Implementation
 *
 * Design based on cyberpunk-style.html:
 * - Cyberpunk aesthetic (cyan/magenta colors)
 * - Connection type (LAN/WiFi) and speed
 * - Real-time traffic graph (Chart widget)
 * - Upload/Download speeds
 */

#include "screens_lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Widget handles and NETWORK_HISTORY_SIZE defined in screens_lvgl.h */

screen_network_t *screen_network_create(lv_obj_t *parent)
{
    screen_network_t *s = malloc(sizeof(screen_network_t));
    if (!s || !parent) {
        free(s);
        return NULL;
    }

    s->history_index = 0;
    s->last_down = SCREEN_VALUE_SENTINEL;
    s->last_up = SCREEN_VALUE_SENTINEL;
    s->last_type[0] = '\0';
    s->last_speed[0] = '\0';

    s->screen = screen_panel_create(parent);

    /* ========================================================================
     * HEADER LABELS
     * ====================================================================== */

    /* "NETWORK.SYS" Header */
    s->label_header = lv_label_create(s->screen);
    lv_label_set_text(s->label_header, "NETWORK");
    lv_obj_set_style_text_font(s->label_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s->label_header, lv_color_make(0x00, 0xff, 0xff), 0);
    lv_obj_align(s->label_header, LV_ALIGN_TOP_MID, 0, 22);

    /* Connection Type (LAN/WiFi) */
    s->label_conn_type = lv_label_create(s->screen);
    lv_label_set_text(s->label_conn_type, "LAN");
    lv_obj_set_style_text_font(s->label_conn_type, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s->label_conn_type, lv_color_make(0x00, 0xff, 0xff), 0);
    lv_obj_align(s->label_conn_type, LV_ALIGN_TOP_MID, 0, 52);

    /* Speed Indicator */
    s->label_speed = lv_label_create(s->screen);
    lv_label_set_text(s->label_speed, "1000 Mbps");
    lv_obj_set_style_text_font(s->label_speed, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s->label_speed, lv_color_make(0xff, 0x00, 0xff), 0);
    lv_obj_align(s->label_speed, LV_ALIGN_TOP_MID, 0, 96);

    /* ========================================================================
     * CHART WIDGET (Traffic Graph)
     * ====================================================================== */
    s->chart = lv_chart_create(s->screen);
    lv_obj_set_size(s->chart, 420, 170);
    lv_obj_align(s->chart, LV_ALIGN_CENTER, 0, 16);

    lv_chart_set_type(s->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s->chart, NETWORK_HISTORY_SIZE);
    lv_chart_set_range(s->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(s->chart, LV_CHART_UPDATE_MODE_SHIFT);

    /* Styling */
    lv_obj_set_style_bg_color(s->chart, lv_color_make(0x00, 0x14, 0x28), 0);
    lv_obj_set_style_border_color(s->chart, lv_color_make(0x00, 0xff, 0xff), 0);
    lv_obj_set_style_border_width(s->chart, 1, 0);

    /* Create series for download (cyan) and upload (magenta) */
    s->ser_down = lv_chart_add_series(s->chart, lv_color_make(0x00, 0xff, 0xff), LV_CHART_AXIS_PRIMARY_Y);
    s->ser_up = lv_chart_add_series(s->chart, lv_color_make(0xff, 0x00, 0xff), LV_CHART_AXIS_PRIMARY_Y);

    /* Initialize with zeros */
    for (int i = 0; i < NETWORK_HISTORY_SIZE; i++) {
        lv_chart_set_next_value(s->chart, s->ser_down, 0);
        lv_chart_set_next_value(s->chart, s->ser_up, 0);
    }

    /* ========================================================================
     * TRAFFIC STATS (Download/Upload) - Centered below chart
     * ====================================================================== */

    /* Download Speed - Center Bottom (first line) */
    s->label_down = lv_label_create(s->screen);
    lv_label_set_text(s->label_down, "DN: 0 MB/s");
    lv_obj_set_style_text_font(s->label_down, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s->label_down, lv_color_make(0x00, 0xff, 0xff), 0);
    lv_obj_set_style_text_align(s->label_down, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s->label_down, LV_ALIGN_BOTTOM_MID, 0, -78);

    /* Upload Speed - Center Bottom (second line) */
    s->label_up = lv_label_create(s->screen);
    lv_label_set_text(s->label_up, "UP: 0 MB/s");
    lv_obj_set_style_text_font(s->label_up, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s->label_up, lv_color_make(0xff, 0x00, 0xff), 0);
    lv_obj_set_style_text_align(s->label_up, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s->label_up, LV_ALIGN_BOTTOM_MID, 0, -32);

    screen_pass_swipe(s->screen);
    return s;
}

/**
 * @brief Get the screen object (for screensaver restore)
 */
lv_obj_t *screen_network_get_screen(screen_network_t *s)
{
    return s ? s->screen : NULL;
}

void screen_network_update(screen_network_t *s, const pc_stats_t *stats)
{
    if (!s) return;

    /* Skip redraw if nothing changed. Critical here: without this the chart
     * shifts at the 10 FPS display rate instead of once per real data packet,
     * making the traffic graph scroll ~10x too fast. */
    if (stats->net_down_mbps == s->last_down && stats->net_up_mbps == s->last_up &&
        strcmp(stats->net_type, s->last_type) == 0 &&
        strcmp(stats->net_speed, s->last_speed) == 0) {
        return;
    }
    s->last_down = stats->net_down_mbps;
    s->last_up = stats->net_up_mbps;
    strncpy(s->last_type, stats->net_type, sizeof(s->last_type) - 1);
    s->last_type[sizeof(s->last_type) - 1] = '\0';
    strncpy(s->last_speed, stats->net_speed, sizeof(s->last_speed) - 1);
    s->last_speed[sizeof(s->last_speed) - 1] = '\0';

    /* Update connection type */
    lv_label_set_text(s->label_conn_type, stats->net_type);

    /* Update speed */
    lv_label_set_text(s->label_speed, stats->net_speed);

    /* ---- Download speed ---- */
    if (stats->net_down_mbps < 0.0f) {
        /* Counter error - show N/A in red */
        lv_label_set_text(s->label_down, "DN: N/A");
        lv_obj_set_style_text_color(s->label_down, lv_color_hex(0xFF4444), 0);
    } else {
    /* Below 1 MB/s the MB figure rounds to 0.0, so show KB/s instead. */
    char down_buf[24];
    if (stats->net_down_mbps < 1.0f) {
        snprintf(down_buf, sizeof(down_buf), "DN: %.0f KB/s", stats->net_down_mbps * 1024.0f);
    } else {
        snprintf(down_buf, sizeof(down_buf), "DN: %.1f MB/s", stats->net_down_mbps);
    }
    lv_label_set_text(s->label_down, down_buf);
        lv_obj_set_style_text_color(s->label_down, lv_color_make(0x00, 0xff, 0xff), 0); /* Cyan */
    }

    /* ---- Upload speed ---- */
    if (stats->net_up_mbps < 0.0f) {
        /* Counter error - show N/A in red */
        lv_label_set_text(s->label_up, "UP: N/A");
        lv_obj_set_style_text_color(s->label_up, lv_color_hex(0xFF4444), 0);
    } else {
    char up_buf[24];
    if (stats->net_up_mbps < 1.0f) {
        snprintf(up_buf, sizeof(up_buf), "UP: %.0f KB/s", stats->net_up_mbps * 1024.0f);
    } else {
        snprintf(up_buf, sizeof(up_buf), "UP: %.1f MB/s", stats->net_up_mbps);
    }
    lv_label_set_text(s->label_up, up_buf);
        lv_obj_set_style_text_color(s->label_up, lv_color_make(0xff, 0x00, 0xff), 0); /* Magenta */
    }

    /* Add new data point to chart (scale to 0-100 range) */
    /* Assuming max 125 MB/s for scaling */
    /* Use 0 for chart if value is negative (error state) */
    float down_val = (stats->net_down_mbps >= 0.0f) ? stats->net_down_mbps : 0.0f;
    float up_val = (stats->net_up_mbps >= 0.0f) ? stats->net_up_mbps : 0.0f;

    /* 5 MB/s fills the chart, so ordinary KB/s traffic is still visible. */
    int down_scaled = (int)((down_val / 5.0f) * 100.0f);
    int up_scaled = (int)((up_val / 5.0f) * 100.0f);

    if (down_scaled > 100) down_scaled = 100;
    if (up_scaled > 100) up_scaled = 100;

    lv_chart_set_next_value(s->chart, s->ser_down, down_scaled);
    lv_chart_set_next_value(s->chart, s->ser_up, up_scaled);

    lv_chart_refresh(s->chart);
}
