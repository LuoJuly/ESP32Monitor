/**
 * @file ui_manager.h
 * @brief UI Manager - Theme and Display Management
 *
 * Centralizes UI operations: theme application, screen updates, screensaver control.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/system_types.h"

/* Forward declarations for screen types */
typedef struct screen_cpu_t screen_cpu_t;
typedef struct screen_gpu_t screen_gpu_t;
typedef struct screen_ram_t screen_ram_t;
typedef struct screen_network_t screen_network_t;
typedef struct screen_weather_t screen_weather_t;
typedef struct screen_clock_t screen_clock_t;

/* Screen handles structure */
typedef struct {
    screen_cpu_t *cpu;
    screen_gpu_t *gpu;
    screen_ram_t *ram;
    screen_network_t *network;
    screen_weather_t *weather;
    screen_clock_t *clock;
} ui_screens_t;

/* Screensaver handles structure */
typedef struct {
    lv_obj_t *cpu;
    lv_obj_t *gpu;
    lv_obj_t *ram;
    lv_obj_t *net;
} ui_screensavers_t;

/* Status dot handles structure */
typedef struct {
    lv_obj_t *cpu;
    lv_obj_t *gpu;
    lv_obj_t *ram;
    lv_obj_t *net;
    lv_obj_t *weather;
    lv_obj_t *clock;
} ui_status_dots_t;

/* =============================================================================
 * THREAD-SAFE LOCKING API (The Iron Gate)
 * ========================================================================== */

/** Default timeout for LVGL lock acquisition (ms) */
#define UI_LOCK_TIMEOUT_MS      200

/**
 * @brief Acquire LVGL mutex with timeout (Fail-Safe)
 *
 * IMPORTANT: Never blocks indefinitely. If lock cannot be acquired,
 * logs a warning and returns false. The caller should skip the UI update.
 *
 * @param timeout_ms Maximum wait time in milliseconds
 * @return true if lock acquired, false on timeout (skip UI update!)
 */
bool ui_acquire_lock(uint32_t timeout_ms);

/**
 * @brief Release LVGL mutex
 *
 * Only call this if ui_acquire_lock() returned true!
 */
void ui_release_lock(void);

/**
 * @brief Convenience macro for lock-protected LVGL operations
 *
 * Usage:
 *   UI_LOCK_SAFE(200) {
 *       lv_label_set_text(...);
 *   }
 *
 * If lock fails, the block is skipped and a warning is logged.
 */
#define UI_LOCK_SAFE(timeout_ms) \
    for (bool _ui_locked = ui_acquire_lock(timeout_ms); _ui_locked; _ui_locked = (ui_release_lock(), false))

/* =============================================================================
 * INITIALIZATION
 * ========================================================================== */

/**
 * @brief Initialize UI manager
 * @param lvgl_mutex LVGL mutex for thread-safe access
 */
void ui_manager_init(SemaphoreHandle_t lvgl_mutex);

/**
 * @brief Register screen handles with UI manager
 */
void ui_manager_set_screens(ui_screens_t *screens);

/**
 * @brief Register screensaver handles with UI manager
 */
void ui_manager_set_screensavers(ui_screensavers_t *screensavers);

/**
 * @brief Register status dot handles with UI manager
 */
void ui_manager_set_status_dots(ui_status_dots_t *dots);

/**
 * @brief Apply current theme to all UI elements
 *
 * Uses gui_settings to update colors on all screens and screensavers.
 * Must be called from LVGL context (with mutex held).
 */
void ui_manager_apply_theme(void);

/**
 * @brief Apply hardware names to screen labels
 *
 * Must be called from LVGL context (with mutex held).
 */
void ui_manager_apply_hardware_names(void);

/**
 * @brief Update all screens with current PC stats
 * @param stats Pointer to PC stats structure
 *
 * Must be called from LVGL context (with mutex held).
 */
void ui_manager_update_screens(const pc_stats_t *stats);

/**
 * @brief Show/hide screensavers
 * @param show true to show, false to hide
 */
void ui_manager_show_screensavers(bool show);

/**
 * @brief Show/hide status dots (stale data indicators)
 * @param show true to show, false to hide
 */
void ui_manager_show_status_dots(bool show);

/**
 * @brief Check if screensaver is currently active
 */
bool ui_manager_is_screensaver_active(void);

/**
 * @brief Set screensaver active state
 */
void ui_manager_set_screensaver_active(bool active);

/**
 * @brief Handle SET_CLR_* color commands
 * @param line Command line
 * @return true if command was handled
 */
bool ui_manager_handle_color_command(const char *line);

/**
 * @brief Create a status dot indicator on a parent object
 * @param parent Parent LVGL object
 * @return Created dot object (hidden by default)
 */
lv_obj_t *ui_manager_create_status_dot(lv_obj_t *parent);

/**
 * @brief Create a screensaver overlay
 * @param parent Parent LVGL object
 * @param bg_color Background color
 * @param icon_src Image source for the centered icon
 * @return Created screensaver overlay (hidden by default)
 */
lv_obj_t *ui_manager_create_screensaver(lv_obj_t *parent, lv_color_t bg_color, const lv_image_dsc_t *icon_src);

/**
 * @brief Create a screensaver overlay with slot tracking for hot-swap
 * @param parent Parent LVGL object
 * @param bg_color Background color
 * @param icon_src Image source for the centered icon
 * @param slot_index Image slot index (0-3) for tracking, -1 to skip tracking
 * @return Created screensaver overlay (hidden by default)
 */
lv_obj_t *ui_manager_create_screensaver_ex(lv_obj_t *parent, lv_color_t bg_color,
                                           const lv_image_dsc_t *icon_src, int slot_index);

/**
 * @brief Callback for screensaver image reload (called by ss_process_updates)
 * @param slot Image slot that was reloaded
 * @param new_dsc New image descriptor
 *
 * This function refreshes the LVGL image widget source pointer after a new
 * image has been uploaded and loaded. Must be called from UI thread.
 */
void ui_manager_on_image_reload(int slot, const lv_image_dsc_t *new_dsc);

#endif /* UI_MANAGER_H */
