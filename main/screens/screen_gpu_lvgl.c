/**
 * @file screen_gpu_lvgl.c
 * @brief GPU Gauge Screen (Display 2) - LVGL Implementation
 *
 * Design based on gpu-gauge.html:
 * - Ring gauge (Arc widget) showing GPU percentage
 * - Gradient: Cyan (#4cc9f0) → Blue (#4361ee)
 * - Center text: "GPU", percentage, temperature, VRAM
 */

#include "screens_lvgl.h"
#include <stdio.h>
#include <stdlib.h>

/* Widget handles defined in screens_lvgl.h */

screen_gpu_t *screen_gpu_create(lv_obj_t *parent)
{
    screen_gpu_t *s = malloc(sizeof(screen_gpu_t));
    if (!s || !parent) {
        free(s);
        return NULL;
    }

    s->last_percent = SCREEN_VALUE_SENTINEL;
    s->last_temp = SCREEN_VALUE_SENTINEL;
    s->last_vram_used = SCREEN_VALUE_SENTINEL;
    s->last_vram_total = SCREEN_VALUE_SENTINEL;

    s->screen = screen_panel_create(parent);

    /* ========================================================================
     * ARC WIDGET (Ring Gauge)
     * ====================================================================== */
    s->arc = lv_arc_create(s->screen);
    lv_obj_set_size(s->arc, 452, 452);
    lv_obj_center(s->arc);

    lv_arc_set_range(s->arc, 0, 100);
    lv_arc_set_value(s->arc, 0);
    lv_arc_set_bg_angles(s->arc, 135, 45);
    lv_arc_set_rotation(s->arc, 0);

    /* Styling - NVIDIA Green (matching Intel Blue style) */
    lv_obj_set_style_arc_width(s->arc, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s->arc, 40, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s->arc, lv_color_hex(0x55555C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->arc, lv_color_hex(0x76B900), LV_PART_INDICATOR);  /* NVIDIA Green */
    lv_obj_set_style_arc_rounded(s->arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s->arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(s->arc, 0, LV_PART_KNOB);

    /* ========================================================================
     * CENTER LABELS
     * ====================================================================== */

    /* GPU Model Title - Top (matching SquareLine: Y=-45) */
    s->label_title = lv_label_create(s->screen);
    lv_obj_set_width(s->label_title, LV_SIZE_CONTENT);
    lv_obj_set_height(s->label_title, LV_SIZE_CONTENT);
    lv_obj_set_x(s->label_title, 0);
    lv_obj_set_y(s->label_title, -128);
    lv_obj_set_align(s->label_title, LV_ALIGN_CENTER);
    lv_label_set_text(s->label_title, "GPU");
    lv_obj_set_style_text_color(s->label_title, lv_color_hex(0x76b900), LV_PART_MAIN | LV_STATE_DEFAULT); // NVIDIA Green
    lv_obj_set_style_text_align(s->label_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s->label_title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(s->label_title, 340);
    lv_label_set_long_mode(s->label_title, LV_LABEL_LONG_MODE_DOTS);

    /* Percentage Value - Center */
    s->label_percent = lv_label_create(s->screen);
    lv_obj_set_width(s->label_percent, LV_SIZE_CONTENT);
    lv_obj_set_height(s->label_percent, LV_SIZE_CONTENT);
    lv_obj_set_align(s->label_percent, LV_ALIGN_CENTER);
    lv_label_set_text(s->label_percent, "XX%");
    lv_obj_set_style_text_color(s->label_percent, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s->label_percent, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s->label_percent, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s->label_percent, &lv_font_montserrat_42, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* VRAM - Center (matching SquareLine: Y=38, Font=22) */
    s->label_vram = lv_label_create(s->screen);
    lv_obj_set_width(s->label_vram, LV_SIZE_CONTENT);
    lv_obj_set_height(s->label_vram, LV_SIZE_CONTENT);
    lv_obj_set_x(s->label_vram, 0);
    lv_obj_set_y(s->label_vram, 62);
    lv_obj_set_align(s->label_vram, LV_ALIGN_CENTER);
    lv_label_set_text(s->label_vram, "12 / 12 GB");
    lv_obj_set_style_text_color(s->label_vram, lv_color_hex(0x4CAF50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s->label_vram, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s->label_vram, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Temperature - Bottom (matching SquareLine: Y=70) */
    s->label_temp = lv_label_create(s->screen);
    lv_obj_set_width(s->label_temp, LV_SIZE_CONTENT);
    lv_obj_set_height(s->label_temp, LV_SIZE_CONTENT);
    lv_obj_set_x(s->label_temp, 0);
    lv_obj_set_y(s->label_temp, 128);
    lv_obj_set_align(s->label_temp, LV_ALIGN_CENTER);
    lv_label_set_text(s->label_temp, "XX°C");
    lv_obj_set_style_text_color(s->label_temp, lv_color_hex(0xF40B0B), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s->label_temp, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s->label_temp, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s->label_temp, &lv_font_montserrat_42, LV_PART_MAIN | LV_STATE_DEFAULT);

    screen_pass_swipe(s->screen);
    return s;
}

/**
 * @brief Get the screen object (for screensaver restore)
 */
lv_obj_t *screen_gpu_get_screen(screen_gpu_t *s)
{
    return s ? s->screen : NULL;
}

void screen_gpu_update(screen_gpu_t *s, const pc_stats_t *stats)
{
    if (!s) return;

    /* Skip redraw if nothing changed (display task runs faster than data arrives) */
    if (stats->gpu_percent == s->last_percent && stats->gpu_temp == s->last_temp &&
        stats->gpu_vram_used == s->last_vram_used && stats->gpu_vram_total == s->last_vram_total) {
        return;
    }
    s->last_percent = stats->gpu_percent;
    s->last_temp = stats->gpu_temp;
    s->last_vram_used = stats->gpu_vram_used;
    s->last_vram_total = stats->gpu_vram_total;

    /* ---- Load ---- */
    if (stats->gpu_percent < 0) {
        lv_arc_set_value(s->arc, 0);
        lv_label_set_text(s->label_percent, "N/A");
        lv_obj_set_style_text_color(s->label_percent, lv_color_hex(0xFF4444), LV_PART_MAIN);
    } else {
        int gpu_val = stats->gpu_percent;
        if (gpu_val > 100) gpu_val = 100;
        lv_arc_set_value(s->arc, gpu_val);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", stats->gpu_percent);
        lv_label_set_text(s->label_percent, buf);
        lv_obj_set_style_text_color(s->label_percent, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    }

    /* ---- Temperature ---- */
    if (stats->gpu_temp < 0.0f) {
        lv_label_set_text(s->label_temp, "N/A");
        lv_obj_set_style_text_color(s->label_temp, lv_color_hex(0xFF4444), 0);
    } else {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", stats->gpu_temp);
        lv_label_set_text(s->label_temp, temp_buf);

        lv_color_t temp_color;
        if (stats->gpu_temp > 75.0f) {
            temp_color = lv_color_hex(0xFF4444);
        } else if (stats->gpu_temp > 65.0f) {
            temp_color = lv_color_hex(0xFF6B6B);
        } else {
            temp_color = lv_color_hex(0x4CAF50);
        }
        lv_obj_set_style_text_color(s->label_temp, temp_color, 0);
    }

    /* ---- VRAM ---- */
    if (stats->gpu_vram_total < 0.0f || stats->gpu_vram_used < 0.0f) {
        lv_label_set_text(s->label_vram, "N/A");
        lv_obj_set_style_text_color(s->label_vram, lv_color_hex(0xFF4444), 0);
    } else {
        char vram_buf[32];
        float total_vram = (stats->gpu_vram_total > 0.1f) ? stats->gpu_vram_total : 1.0f;
        snprintf(vram_buf, sizeof(vram_buf), "%.1f / %.0f GB",
                 stats->gpu_vram_used, total_vram);
        lv_label_set_text(s->label_vram, vram_buf);
        lv_obj_set_style_text_color(s->label_vram, lv_color_hex(0x4CAF50), 0);
    }
}
