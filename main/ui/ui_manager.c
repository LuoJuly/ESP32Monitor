/**
 * @file ui_manager.c
 * @brief UI Manager Implementation
 */

#include "ui_manager.h"
#include "screensaver_mgr.h"
#include "../gui_settings.h"
#include "../storage/hw_identity.h"
#include "../screens/screens_lvgl.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "UI-MGR";

/* Screen references */
static ui_screens_t *s_screens = NULL;
static ui_screensavers_t *s_screensavers = NULL;
static ui_status_dots_t *s_dots = NULL;
static bool s_screensaver_active = false;

/* Screensaver image widget handles (for hot-swap updates) */
static lv_obj_t *s_ss_images[4] = {NULL, NULL, NULL, NULL};

/* Lock statistics for debugging */
static uint32_t s_lock_timeouts = 0;
static uint32_t s_lock_successes = 0;

/* =============================================================================
 * THREAD-SAFE LOCKING API (The Iron Gate)
 * ========================================================================== */

bool ui_acquire_lock(uint32_t timeout_ms)
{
    /* Same mutex the Waveshare LVGL adapter task uses. */
    if (bsp_display_lock(timeout_ms) == ESP_OK) {
        s_lock_successes++;
        return true;
    }

    s_lock_timeouts++;
    ESP_LOGW(TAG, "UI Lock Timeout (%lu ms)! Skipping update. [timeouts: %lu, successes: %lu]",
             (unsigned long)timeout_ms, (unsigned long)s_lock_timeouts, (unsigned long)s_lock_successes);
    return false;
}

void ui_release_lock(void)
{
    bsp_display_unlock();
}

/* =============================================================================
 * INITIALIZATION
 * ========================================================================== */

void ui_manager_init(SemaphoreHandle_t lvgl_mutex)
{
    (void)lvgl_mutex;
    ESP_LOGI(TAG, "UI Manager initialized");
}

void ui_manager_set_screens(ui_screens_t *screens)
{
    s_screens = screens;
}

void ui_manager_set_screensavers(ui_screensavers_t *screensavers)
{
    s_screensavers = screensavers;
}

void ui_manager_set_status_dots(ui_status_dots_t *dots)
{
    s_dots = dots;
}

/* =============================================================================
 * THEME APPLICATION
 * ========================================================================== */

void ui_manager_apply_theme(void)
{
    ESP_LOGI(TAG, "Applying theme...");

    /* --- CPU Screen --- */
    if (s_screens && s_screens->cpu) {
        screen_cpu_t *scr = s_screens->cpu;
        if (scr->screen) {
            lv_obj_set_style_bg_color(scr->screen,
                lv_color_hex(gui_settings.bg_color[SCREEN_CPU]), 0);
        }
        if (scr->arc) {
            lv_obj_set_style_arc_color(scr->arc,
                lv_color_hex(gui_settings.arc_bg_color), LV_PART_MAIN);
            lv_obj_set_style_arc_color(scr->arc,
                lv_color_hex(gui_settings.arc_color_cpu), LV_PART_INDICATOR);
        }
        if (scr->label_title) {
            lv_obj_set_style_text_color(scr->label_title,
                lv_color_hex(gui_settings.text_title_cpu), LV_PART_MAIN);
        }
    }

    /* --- GPU Screen --- */
    if (s_screens && s_screens->gpu) {
        screen_gpu_t *scr = s_screens->gpu;
        if (scr->screen) {
            lv_obj_set_style_bg_color(scr->screen,
                lv_color_hex(gui_settings.bg_color[SCREEN_GPU]), 0);
        }
        if (scr->arc) {
            lv_obj_set_style_arc_color(scr->arc,
                lv_color_hex(gui_settings.arc_bg_color), LV_PART_MAIN);
            lv_obj_set_style_arc_color(scr->arc,
                lv_color_hex(gui_settings.arc_color_gpu), LV_PART_INDICATOR);
        }
        if (scr->label_title) {
            lv_obj_set_style_text_color(scr->label_title,
                lv_color_hex(gui_settings.text_title_gpu), LV_PART_MAIN);
        }
    }

    /* --- RAM Screen --- */
    if (s_screens && s_screens->ram) {
        screen_ram_t *scr = s_screens->ram;
        if (scr->screen) {
            lv_obj_set_style_bg_color(scr->screen,
                lv_color_hex(gui_settings.bg_color[SCREEN_RAM]), 0);
        }
        if (scr->bar) {
            lv_obj_set_style_bg_color(scr->bar,
                lv_color_hex(gui_settings.bar_bg_color), LV_PART_MAIN);
        }
    }

    /* --- Network Screen --- */
    if (s_screens && s_screens->network) {
        screen_network_t *scr = s_screens->network;
        if (scr->screen) {
            lv_obj_set_style_bg_color(scr->screen,
                lv_color_hex(gui_settings.bg_color[SCREEN_NET]), 0);
        }
        if (scr->chart) {
            lv_obj_set_style_bg_color(scr->chart,
                lv_color_hex(gui_settings.net_chart_bg), 0);
            lv_obj_set_style_border_color(scr->chart,
                lv_color_hex(gui_settings.net_chart_border), 0);
        }
        if (scr->label_header) {
            lv_obj_set_style_text_color(scr->label_header,
                lv_color_hex(gui_settings.text_title_net), 0);
        }
    }

    /* --- Screensaver backgrounds --- */
    if (s_screensavers) {
        if (s_screensavers->cpu) {
            lv_obj_set_style_bg_color(s_screensavers->cpu,
                lv_color_hex(gui_settings.ss_bg_color[SCREEN_CPU]), 0);
        }
        if (s_screensavers->gpu) {
            lv_obj_set_style_bg_color(s_screensavers->gpu,
                lv_color_hex(gui_settings.ss_bg_color[SCREEN_GPU]), 0);
        }
        if (s_screensavers->ram) {
            lv_obj_set_style_bg_color(s_screensavers->ram,
                lv_color_hex(gui_settings.ss_bg_color[SCREEN_RAM]), 0);
        }
        if (s_screensavers->net) {
            lv_obj_set_style_bg_color(s_screensavers->net,
                lv_color_hex(gui_settings.ss_bg_color[SCREEN_NET]), 0);
        }
    }

    ESP_LOGI(TAG, "Theme applied");
}

/* =============================================================================
 * HARDWARE NAMES
 * ========================================================================== */

void ui_manager_apply_hardware_names(void)
{
    hw_identity_t *id = hw_identity_get();

    if (s_screens) {
        if (s_screens->cpu && s_screens->cpu->label_title) {
            lv_label_set_text(s_screens->cpu->label_title, id->cpu_name);
        }
        if (s_screens->gpu && s_screens->gpu->label_title) {
            lv_label_set_text(s_screens->gpu->label_title, id->gpu_name);
        }
    }
}

/* =============================================================================
 * SCREEN UPDATES
 * ========================================================================== */

void ui_manager_update_screens(const pc_stats_t *stats)
{
    if (!s_screens || !stats) return;

    if (s_screens->cpu) screen_cpu_update(s_screens->cpu, stats);
    if (s_screens->gpu) screen_gpu_update(s_screens->gpu, stats);
    if (s_screens->ram) screen_ram_update(s_screens->ram, stats);
    if (s_screens->network) screen_network_update(s_screens->network, stats);
    if (s_screens->weather) screen_weather_update(s_screens->weather, stats);
    if (s_screens->clock) screen_clock_update(s_screens->clock, stats);
}

/* =============================================================================
 * SCREENSAVER CONTROL
 * ========================================================================== */

void ui_manager_show_screensavers(bool show)
{
    if (!s_screensavers) return;

    if (show) {
        if (s_screensavers->cpu) lv_obj_clear_flag(s_screensavers->cpu, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->gpu) lv_obj_clear_flag(s_screensavers->gpu, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->ram) lv_obj_clear_flag(s_screensavers->ram, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->net) lv_obj_clear_flag(s_screensavers->net, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_screensavers->cpu) lv_obj_add_flag(s_screensavers->cpu, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->gpu) lv_obj_add_flag(s_screensavers->gpu, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->ram) lv_obj_add_flag(s_screensavers->ram, LV_OBJ_FLAG_HIDDEN);
        if (s_screensavers->net) lv_obj_add_flag(s_screensavers->net, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_manager_show_status_dots(bool show)
{
    if (!s_dots) return;

    if (show) {
        if (s_dots->cpu) lv_obj_clear_flag(s_dots->cpu, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->gpu) lv_obj_clear_flag(s_dots->gpu, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->ram) lv_obj_clear_flag(s_dots->ram, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->net) lv_obj_clear_flag(s_dots->net, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->weather) lv_obj_clear_flag(s_dots->weather, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->clock) lv_obj_clear_flag(s_dots->clock, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_dots->cpu) lv_obj_add_flag(s_dots->cpu, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->gpu) lv_obj_add_flag(s_dots->gpu, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->ram) lv_obj_add_flag(s_dots->ram, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->net) lv_obj_add_flag(s_dots->net, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->weather) lv_obj_add_flag(s_dots->weather, LV_OBJ_FLAG_HIDDEN);
        if (s_dots->clock) lv_obj_add_flag(s_dots->clock, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_manager_is_screensaver_active(void)
{
    return s_screensaver_active;
}

void ui_manager_set_screensaver_active(bool active)
{
    s_screensaver_active = active;
}

/* =============================================================================
 * COLOR COMMAND HANDLER
 * ========================================================================== */

static uint32_t parse_hex_color(const char *hex_str)
{
    if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
    }
    return (uint32_t)strtoul(hex_str, NULL, 16);
}

bool ui_manager_handle_color_command(const char *line)
{
    bool needs_save = false;
    bool needs_theme_update = false;

    /* SET_CLR_ARC_CPU:RRGGBB */
    if (strncmp(line, "SET_CLR_ARC_CPU:", 16) == 0) {
        gui_settings.arc_color_cpu = parse_hex_color(line + 16);
        ESP_LOGI(TAG, "Set CPU arc color: 0x%06lX", (unsigned long)gui_settings.arc_color_cpu);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_ARC_GPU:RRGGBB */
    else if (strncmp(line, "SET_CLR_ARC_GPU:", 16) == 0) {
        gui_settings.arc_color_gpu = parse_hex_color(line + 16);
        ESP_LOGI(TAG, "Set GPU arc color: 0x%06lX", (unsigned long)gui_settings.arc_color_gpu);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_ARC_BG:RRGGBB */
    else if (strncmp(line, "SET_CLR_ARC_BG:", 15) == 0) {
        gui_settings.arc_bg_color = parse_hex_color(line + 15);
        ESP_LOGI(TAG, "Set arc bg color: 0x%06lX", (unsigned long)gui_settings.arc_bg_color);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_BAR_RAM:RRGGBB */
    else if (strncmp(line, "SET_CLR_BAR_RAM:", 16) == 0) {
        gui_settings.bar_color_ram = parse_hex_color(line + 16);
        ESP_LOGI(TAG, "Set RAM bar color: 0x%06lX", (unsigned long)gui_settings.bar_color_ram);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_NET_DN:RRGGBB */
    else if (strncmp(line, "SET_CLR_NET_DN:", 15) == 0) {
        gui_settings.net_color_down = parse_hex_color(line + 15);
        ESP_LOGI(TAG, "Set net download color: 0x%06lX", (unsigned long)gui_settings.net_color_down);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_NET_UP:RRGGBB */
    else if (strncmp(line, "SET_CLR_NET_UP:", 15) == 0) {
        gui_settings.net_color_up = parse_hex_color(line + 15);
        ESP_LOGI(TAG, "Set net upload color: 0x%06lX", (unsigned long)gui_settings.net_color_up);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_TXT_TITLE:Index:RRGGBB */
    else if (strncmp(line, "SET_CLR_TXT_TITLE:", 18) == 0) {
        int idx = line[18] - '0';
        if (idx >= 0 && idx < 4 && line[19] == ':') {
            uint32_t color = parse_hex_color(line + 20);
            switch (idx) {
                case 0: gui_settings.text_title_cpu = color; break;
                case 1: gui_settings.text_title_gpu = color; break;
                case 2: gui_settings.text_title_ram = color; break;
                case 3: gui_settings.text_title_net = color; break;
            }
            ESP_LOGI(TAG, "Set title color[%d]: 0x%06lX", idx, (unsigned long)color);
            needs_save = needs_theme_update = true;
        }
    }
    /* SET_CLR_TXT_VAL:RRGGBB */
    else if (strncmp(line, "SET_CLR_TXT_VAL:", 16) == 0) {
        gui_settings.text_value = parse_hex_color(line + 16);
        ESP_LOGI(TAG, "Set value text color: 0x%06lX", (unsigned long)gui_settings.text_value);
        needs_save = needs_theme_update = true;
    }
    /* SET_CLR_BG_NORM:Index:RRGGBB */
    else if (strncmp(line, "SET_CLR_BG_NORM:", 16) == 0) {
        int idx = line[16] - '0';
        if (idx >= 0 && idx < 4 && line[17] == ':') {
            gui_settings.bg_color[idx] = parse_hex_color(line + 18);
            ESP_LOGI(TAG, "Set bg color[%d]: 0x%06lX", idx, (unsigned long)gui_settings.bg_color[idx]);
            needs_save = needs_theme_update = true;
        }
    }
    /* SET_CLR_BG_SS:Index:RRGGBB */
    else if (strncmp(line, "SET_CLR_BG_SS:", 14) == 0) {
        int idx = line[14] - '0';
        if (idx >= 0 && idx < 4 && line[15] == ':') {
            gui_settings.ss_bg_color[idx] = parse_hex_color(line + 16);
            ESP_LOGI(TAG, "Set screensaver bg[%d]: 0x%06lX", idx, (unsigned long)gui_settings.ss_bg_color[idx]);
            needs_save = needs_theme_update = true;
        }
    }
    /* SET_CLR_TEMP:Type:RRGGBB */
    else if (strncmp(line, "SET_CLR_TEMP:", 13) == 0) {
        int idx = line[13] - '0';
        if (idx >= 0 && idx < 3 && line[14] == ':') {
            uint32_t color = parse_hex_color(line + 15);
            switch (idx) {
                case 0: gui_settings.temp_cold = color; break;
                case 1: gui_settings.temp_warm = color; break;
                case 2: gui_settings.temp_hot = color; break;
            }
            ESP_LOGI(TAG, "Set temp color[%d]: 0x%06lX", idx, (unsigned long)color);
            needs_save = true;
        }
    }
    /* RESET_THEME */
    else if (strcmp(line, "RESET_THEME") == 0) {
        gui_settings_init_defaults(&gui_settings);
        ESP_LOGI(TAG, "Reset to default Desert-Spec theme");
        needs_save = needs_theme_update = true;
    }
    else {
        return false;  /* Not a color command */
    }

    /* Save to LittleFS if needed */
    if (needs_save) {
        gui_settings_save();
    }

    /* Apply theme if needed (must be done in LVGL context) */
    if (needs_theme_update) {
        if (ui_acquire_lock(100)) {
            ui_manager_apply_theme();
            ui_release_lock();
        }
    }

    return true;
}

/* =============================================================================
 * UI ELEMENT CREATORS
 * ========================================================================== */

lv_obj_t *ui_manager_create_status_dot(lv_obj_t *parent)
{
    if (!parent) return NULL;

    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    /* Below the top of the circle so it clears the title, still inside the ring. */
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -156);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

    return dot;
}

lv_obj_t *ui_manager_create_screensaver_ex(lv_obj_t *parent, lv_color_t bg_color,
                                           const lv_image_dsc_t *icon_src, int slot_index)
{
    if (!parent) return NULL;

    /* Fullscreen overlay container */
    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, bg_color, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered image */
    lv_obj_t *img = lv_img_create(overlay);
    lv_img_set_src(img, icon_src);
    lv_obj_center(img);

    /* Store image handle for hot-swap updates */
    if (slot_index >= 0 && slot_index < 4) {
        s_ss_images[slot_index] = img;
    }

    /* Start hidden */
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    return overlay;
}

lv_obj_t *ui_manager_create_screensaver(lv_obj_t *parent, lv_color_t bg_color, const lv_image_dsc_t *icon_src)
{
    /* Legacy wrapper - doesn't store image handle */
    return ui_manager_create_screensaver_ex(parent, bg_color, icon_src, -1);
}

/* =============================================================================
 * SCREENSAVER IMAGE HOT-SWAP (Thread-Safe Refresh)
 *
 * This callback is called by ss_process_updates() when an image has been
 * reloaded. It updates the LVGL image widget with the new source pointer.
 * ========================================================================== */

void ui_manager_on_image_reload(int slot, const lv_image_dsc_t *new_dsc)
{
    if (slot < 0 || slot >= 4 || !new_dsc) return;

    lv_obj_t *img = s_ss_images[slot];
    if (img) {
        lv_img_set_src(img, new_dsc);
        ESP_LOGI(TAG, "Screensaver slot %d image source refreshed", slot);
    } else {
        ESP_LOGW(TAG, "Screensaver slot %d image handle not found", slot);
    }
}
