/**
 * @file screen_ram_lvgl.c
 * @brief RAM Bar Screen (Display 3) - LVGL Implementation
 *
 * Design based on ram-bars.html:
 * - Horizontal progress bar (Bar widget)
 * - Gradient: Green (#43e97b) → Turquoise (#38f9d7)
 * - Segments for visual clarity
 * - Shows RAM used, percentage, total
 */

#include "screens_lvgl.h"
#include <stdio.h>
#include <stdlib.h>

/* Widget handles defined in screens_lvgl.h */

screen_ram_t *screen_ram_create(lv_obj_t *parent)
{
    screen_ram_t *s = malloc(sizeof(screen_ram_t));
    if (!s || !parent) {
        free(s);
        return NULL;
    }

    s->last_used = SCREEN_VALUE_SENTINEL;
    s->last_total = SCREEN_VALUE_SENTINEL;

    s->screen = screen_panel_create(parent);

    /* ========================================================================
     * LABELS
     * ====================================================================== */

    /* "RAM" Title */
    s->label_title = lv_label_create(s->screen);
    lv_label_set_text(s->label_title, "RAM");
    lv_obj_set_style_text_color(s->label_title, lv_color_hex(0xECE81A), LV_PART_MAIN | LV_STATE_DEFAULT); // Corsair Yellow
    lv_obj_set_style_text_font(s->label_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s->label_title, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_align(s->label_title, LV_ALIGN_TOP_MID, 0, 28);

    /* Used RAM Value */
    s->label_value = lv_label_create(s->screen);
    lv_label_set_text(s->label_value, "0.0 GB");
    lv_obj_set_style_text_font(s->label_value, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(s->label_value, lv_color_white(), 0);
    lv_obj_align(s->label_value, LV_ALIGN_CENTER, 0, -40);

    /* Percentage */
    s->label_percent = lv_label_create(s->screen);
    lv_label_set_text(s->label_percent, "0%");
    lv_obj_set_style_text_font(s->label_percent, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s->label_percent, lv_color_make(0x43, 0xe9, 0x7b), 0);
    lv_obj_align(s->label_percent, LV_ALIGN_CENTER, 0, 20);

    /* ========================================================================
     * PROGRESS BAR
     * ====================================================================== */
    s->bar = lv_bar_create(s->screen);
    lv_obj_set_size(s->bar, 430, 48);
    lv_obj_align(s->bar, LV_ALIGN_CENTER, 0, 70);

    lv_bar_set_range(s->bar, 0, 100);
    lv_bar_set_value(s->bar, 0, LV_ANIM_OFF);

    /* Styling */
    lv_obj_set_style_bg_color(s->bar, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->bar, lv_color_make(0x43, 0xe9, 0x7b), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s->bar, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(s->bar, 15, LV_PART_INDICATOR);

    /* Total RAM */
    s->label_total = lv_label_create(s->screen);
    lv_label_set_text(s->label_total, "von 64 GB");
    lv_obj_set_style_text_font(s->label_total, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s->label_total, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_align(s->label_total, LV_ALIGN_CENTER, 0, 140);

    screen_pass_swipe(s->screen);
    return s;
}

/**
 * @brief Get the screen object (for screensaver restore)
 */
lv_obj_t *screen_ram_get_screen(screen_ram_t *s)
{
    return s ? s->screen : NULL;
}

void screen_ram_update(screen_ram_t *s, const pc_stats_t *stats)
{
    if (!s) return;

    /* Skip redraw if nothing changed (display task runs faster than data arrives) */
    if (stats->ram_used_gb == s->last_used && stats->ram_total_gb == s->last_total) {
        return;
    }
    s->last_used = stats->ram_used_gb;
    s->last_total = stats->ram_total_gb;

    /* ---- Check for sensor error (N/A case) ---- */
    if (stats->ram_used_gb < 0.0f || stats->ram_total_gb < 0.0f) {
        /* Sensor error - show N/A in red */
        lv_bar_set_value(s->bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s->label_value, "N/A");
        lv_obj_set_style_text_color(s->label_value, lv_color_hex(0xFF4444), 0);
        lv_label_set_text(s->label_percent, "N/A");
        lv_obj_set_style_text_color(s->label_percent, lv_color_hex(0xFF4444), 0);
        lv_label_set_text(s->label_total, "");
        lv_obj_set_style_bg_color(s->bar, lv_color_hex(0xFF4444), LV_PART_INDICATOR);
        return;
    }

    /* ---- Valid data ---- */

    /* Calculate percentage (guard against division by zero at boot) */
    int percent;
    if (stats->ram_total_gb > 0.1f) {
        percent = (int)((stats->ram_used_gb / stats->ram_total_gb) * 100.0f);
    } else {
        percent = 0;
    }
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;

    /* Update bar */
    lv_bar_set_value(s->bar, percent, LV_ANIM_ON);

    /* Update used RAM value */
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f GB", stats->ram_used_gb);
    lv_label_set_text(s->label_value, buf);
    lv_obj_set_style_text_color(s->label_value, lv_color_white(), 0);

    /* Update percentage */
    char percent_buf[8];
    snprintf(percent_buf, sizeof(percent_buf), "%d%%", percent);
    lv_label_set_text(s->label_percent, percent_buf);
    lv_obj_set_style_text_color(s->label_percent, lv_color_make(0x43, 0xe9, 0x7b), 0);

    /* Update total */
    char total_buf[16];
    snprintf(total_buf, sizeof(total_buf), "von %.0f GB", stats->ram_total_gb);
    lv_label_set_text(s->label_total, total_buf);

    /* Change bar color based on usage */
    lv_color_t bar_color;
    if (percent > 85) {
        bar_color = lv_color_make(0xff, 0x44, 0x44); /* Red */
    } else if (percent > 70) {
        bar_color = lv_color_make(0xff, 0xa5, 0x00); /* Orange */
    } else {
        bar_color = lv_color_make(0x43, 0xe9, 0x7b); /* Green */
    }
    lv_obj_set_style_bg_color(s->bar, bar_color, LV_PART_INDICATOR);
}
