/**
 * @file main_lvgl.c
 * @brief PC Monitor - Desert-Spec v2.3
 *
 * v2.5 - Waveshare ESP32-S3-Touch-AMOLED-1.75C:
 * - One 466x466 CO5300 QSPI panel (official BSP)
 * - CPU / GPU / RAM / Network are four full pages
 * - Swipe left/right to change page
 * - No data: the page stays up for 10 seconds, then the panel turns off
 * - Tap or short-press BOOT (GPIO0) to wake and wait for data again
 * - CST9217 touch is brought up by the BSP
 *
 * - SET_SS_BG command for per-slot background colors
 * - Transparent PNG overlay on configurable background
 * - Colors persist to LittleFS
 *
 * v2.2 - Image Upload System:
 * - SCARAB image format with 16-byte header
 * - RGB565A8 PLANAR format for LVGL v9
 * - CRC32 verification, chunked USB transfer
 * - Pause/Resume for manual transmission control
 *
 * v2.1 - Thread-Safety Hardening:
 * - NEVER use portMAX_DELAY (causes freezes)
 * - All mutex operations have timeouts with fail-safe skip behavior
 * - Task Watchdog (TWDT) triggers hard reset after 5s hang
 * - Increased stack sizes for safety margin
 * - Proper task priority ordering
 *
 * Modular architecture:
 * - storage/   : LittleFS, hw_identity, gui_settings
 * - drivers/   : usb_serial_comm
 * - ui/        : ui_manager, screensaver_mgr
 * - screens/   : screen implementations
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "src/indev/lv_indev_private.h"
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"

/* Modular includes */
#include "core/system_types.h"
#include "storage/storage_mgr.h"
#include "storage/hw_identity.h"
#include "gui_settings.h"
#include "drivers/usb_serial_comm.h"
#include "drivers/ble_link.h"
#include "drivers/display_board.h"
#include "drivers/power_axp.h"
#include "drivers/fw_update.h"
#include "ui/ui_manager.h"
#include "ui/screensaver_mgr.h"
#include "screens/screens_lvgl.h"

static const char *TAG = "MAIN";

/* =============================================================================
 * CONFIGURATION
 * ========================================================================== */
#define NO_DATA_BLANK_MS         10000   /* No data: keep the page up, then turn the panel off */
#define STALE_DATA_THRESHOLD_MS  4500    /* 4.5s -> show red dot. Client sends
                                          * ~1/s, but a slow GetStats() cycle
                                          * (LibreHardwareMonitor) plus one held
                                          * packet can briefly exceed 3s; 4.5s
                                          * stops the red dot from flickering
                                          * during normal operation. */
#define DISPLAY_UPDATE_MS        100     /* 10 FPS - Watchdog friendly */
#define BURN_IN_SHIFT_MS         60000   /* Nudge the picture 1 px once a minute */

/* =============================================================================
 * DESERT-SPEC: THREAD-SAFETY CONFIGURATION
 * - Never use portMAX_DELAY (can cause freezes)
 * - Use timeouts with fail-safe skip behavior
 * ========================================================================== */
#define LVGL_MUTEX_TIMEOUT_MS    200     /* Max wait for LVGL mutex */
#define STATS_MUTEX_TIMEOUT_MS   100     /* Max wait for stats mutex */

/* =============================================================================
 * TASK STACK SIZES (increased 30% for safety margin)
 * - String operations and printf consume significant stack
 * ========================================================================== */
#define STACK_SIZE_USB_RX        6144
/* Blank and wake call lv_refr_now. A full frame needs more than the old 6 KB. */
#define STACK_SIZE_DISPLAY_UPD   12288

/* =============================================================================
 * TASK PRIORITIES (Higher number = higher priority)
 * Priority order: USB RX > Display Update
 * LVGL rendering runs inside the BSP adapter task.
 * ========================================================================== */
#define PRIO_USB_RX              4       /* Highest - input must not be lost */
#define PRIO_DISPLAY_UPDATE      2       /* Medium - UI updates */

/* =============================================================================
 * WATCHDOG CONFIGURATION
 * ========================================================================== */
#define TWDT_TIMEOUT_SEC         5       /* Task watchdog timeout */

/* =============================================================================
 * GLOBAL STATE
 * ========================================================================== */
static SemaphoreHandle_t s_stats_mutex = NULL;

#define PAGE_COUNT 7
#define PAGE_CLOCK 1
#define PAGE_CPU 3
#define BOOT_GPIO  GPIO_NUM_0

/* Screen Handles */
static ui_screens_t s_screens = {0};
static ui_screensavers_t s_screensavers = {0};
static ui_status_dots_t s_dots = {0};

static lv_obj_t *s_tileview = NULL;
static lv_obj_t *s_page_dot[PAGE_COUNT] = {0};
static lv_obj_t *s_blank_cover = NULL;
static int s_page_index = 0;
static bool s_blanked = false;
static bool s_blank_manual = false;
/* PC_SCREEN from the plugin: -1 none yet, 0 panel off, 1 panel on. */
static volatile int s_pc_panel = -1;
static int s_pc_panel_applied = -1;
/* 1 = BOOT toggles the panel, 2 = a tap wakes it. Handled by the display task. */
static volatile int s_panel_req = 0;
static uint32_t s_wake_ms = 0;
/* 0 = no auto page change. Settings (page 0) is never part of the cycle. */
static uint16_t s_rotate_s = 0;
static uint32_t s_rotate_ms = 0;
/* Finger is down. Set from the touch event, not the 100 ms display poll. */
static bool s_finger = false;

/* One-pixel loop around the origin. Keeps static text off the same OLED pixels. */
static const int8_t s_shift_path[][2] = {
    {0, 0}, {1, 0}, {1, 1}, {0, 1},
    {-1, 1}, {-1, 0}, {-1, -1}, {0, -1},
};

static void burn_in_shift_apply(int8_t x, int8_t y)
{
    if (s_tileview) {
        lv_obj_set_style_translate_x(s_tileview, x, 0);
        lv_obj_set_style_translate_y(s_tileview, y, 0);
    }
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!s_page_dot[i]) continue;
        lv_obj_set_style_translate_x(s_page_dot[i], x, 0);
        lv_obj_set_style_translate_y(s_page_dot[i], y, 0);
    }
}

static void burn_in_shift_poll(uint32_t now)
{
    static uint32_t last_ms;
    static uint8_t step;
    if (last_ms == 0) last_ms = now;
    if ((uint32_t)(now - last_ms) < BURN_IN_SHIFT_MS) return;
    if (s_tileview && lv_obj_is_scrolling(s_tileview)) return;
    last_ms = now;
    step = (uint8_t)((step + 1) % (sizeof(s_shift_path) / sizeof(s_shift_path[0])));
    burn_in_shift_apply(s_shift_path[step][0], s_shift_path[step][1]);
}

#define PAGE_DOT_MS 5000
static uint32_t s_dots_ms;
static bool s_dots_on = true;

static void page_dots_set_visible(bool on)
{
    s_dots_on = on;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!s_page_dot[i]) continue;
        if (on) lv_obj_remove_flag(s_page_dot[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_page_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_dots_poke(uint32_t now)
{
    s_dots_ms = now;
    if (!s_dots_on) page_dots_set_visible(true);
}

static void page_dots_poll(uint32_t now)
{
    if (!s_dots_on || s_blanked) return;
    if (s_finger || (s_tileview && lv_obj_is_scrolling(s_tileview))) {
        s_dots_ms = now;
        return;
    }
    if (s_dots_ms == 0) s_dots_ms = now;
    if ((uint32_t)(now - s_dots_ms) >= PAGE_DOT_MS) page_dots_set_visible(false);
}

static void page_persist(int index);

static void page_dots_sync(int index)
{
    if (index < 0 || index >= PAGE_COUNT) return;
    s_page_index = index;
    page_persist(index);
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!s_page_dot[i]) continue;
        lv_obj_set_style_bg_opa(s_page_dot[i], i == index ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    page_dots_poke((uint32_t)(esp_timer_get_time() / 1000));
}

static void tile_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!s_tileview) return;
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tileview);
    if (!tile) return;
    page_dots_sync((int)(intptr_t)lv_obj_get_user_data(tile));
    s_rotate_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Caller holds the LVGL lock. Panel output is stopped; the touch task keeps running. */
static void pause_panel_refresh(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_timer_t *refr = disp ? lv_display_get_refr_timer(disp) : NULL;
    if (refr) lv_timer_pause(refr);
}

static void resume_panel_refresh(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_timer_t *refr = disp ? lv_display_get_refr_timer(disp) : NULL;
    if (refr) lv_timer_resume(refr);
    if (disp) lv_obj_invalidate(lv_display_get_screen_active(disp));
}

/* Caller holds the LVGL lock. */
static void blank_screen(bool manual)
{
    if (s_blanked) {
        if (manual) s_blank_manual = true;
        return;
    }
    if (s_blank_cover) {
        lv_obj_remove_flag(s_blank_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_blank_cover);
    }
    /* Push the black cover, then stop scanning. Brightness 0 leaves the
     * CO5300 refreshing and it flashes white. */
    lv_refr_now(lv_display_get_default());
    pause_panel_refresh();
    display_board_set_awake(false);
    s_blanked = true;
    s_blank_manual = manual;
}

/* Caller holds the LVGL lock. */
static void wake_screen(void)
{
    if (!s_blanked) return;
    if (s_blank_cover) {
        lv_obj_add_flag(s_blank_cover, LV_OBJ_FLAG_HIDDEN);
    }
    resume_panel_refresh();
    lv_refr_now(lv_display_get_default());
    display_board_set_awake(true);
    s_blanked = false;
    s_blank_manual = false;
    s_wake_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_rotate_ms = s_wake_ms;
    /* Ask the plugin for a fresh sample. The port stays open while the panel is off. */
    usb_serial_send("NEED_DATA\n");
}

static void blank_cover_pressed(lv_event_t *e)
{
    LV_UNUSED(e);
    s_panel_req = 2;
}

/* USB RX task. The display task applies this while it holds the LVGL lock. */
static bool handle_pc_screen_cmd(const char *line)
{
    if (strncmp(line, "PC_SCREEN:", 10) != 0) return false;
    if (line[10] == '0') s_pc_panel = 0;
    else if (line[10] == '1') s_pc_panel = 1;
    return true;
}

static lv_obj_t *add_page(int index, lv_dir_t dir)
{
    lv_obj_t *tile = lv_tileview_add_tile(s_tileview, index, 0, dir);
    lv_obj_set_user_data(tile, (void *)(intptr_t)index);
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    return tile;
}

#define BRIGHTNESS_PATH "/storage/brightness.bin"
#define ROTATE_PATH     "/storage/rotate.bin"
#define PAGE_PATH       "/storage/page.bin"
#define ROTATE_STEP_S   30
#define ROTATE_MAX_S    3600

static lv_obj_t *s_rotate_value;

static uint16_t rotate_normalize(int seconds)
{
    if (seconds <= 0) return 0;
    if (seconds > ROTATE_MAX_S) return ROTATE_MAX_S;
    if (seconds < ROTATE_STEP_S) return ROTATE_STEP_S;
    return (uint16_t)((seconds / ROTATE_STEP_S) * ROTATE_STEP_S);
}

static uint16_t rotate_load(void)
{
    FILE *f = fopen(ROTATE_PATH, "rb");
    if (!f) return 0;
    uint16_t seconds = 0;
    size_t n = fread(&seconds, 1, sizeof(seconds), f);
    fclose(f);
    if (n != sizeof(seconds)) return 0;
    return rotate_normalize(seconds);
}

static void rotate_save(uint16_t seconds)
{
    FILE *f = fopen(ROTATE_PATH, "wb");
    if (!f) return;
    fwrite(&seconds, 1, sizeof(seconds), f);
    fclose(f);
}

/* Last page the strip actually stopped on. -1 until the first write this boot. */
static int s_page_saved = -1;

static int page_load(void)
{
    FILE *f = fopen(PAGE_PATH, "rb");
    if (!f) return PAGE_CPU;
    int c = fgetc(f);
    fclose(f);
    if (c < 0 || c >= PAGE_COUNT) return PAGE_CPU;
    return c;
}

static void page_persist(int index)
{
    if (index == s_page_saved || index < 0 || index >= PAGE_COUNT) return;
    FILE *f = fopen(PAGE_PATH, "wb");
    if (!f) return;
    fputc(index, f);
    fclose(f);
    s_page_saved = index;
}

static void rotate_value_set(int seconds)
{
    char buf[16];
    if (seconds <= 0) snprintf(buf, sizeof(buf), "0s");
    else if (seconds < 60) snprintf(buf, sizeof(buf), "%ds", seconds);
    else if (seconds >= ROTATE_MAX_S) snprintf(buf, sizeof(buf), "1 h");
    else if (seconds % 60 == 0) snprintf(buf, sizeof(buf), "%d min", seconds / 60);
    else snprintf(buf, sizeof(buf), "%dm %ds", seconds / 60, seconds % 60);
    if (s_rotate_value) lv_label_set_text(s_rotate_value, buf);
}

/* Scroll position and finger point when the press started. */
static int32_t s_gesture_x;
static lv_point_t s_gesture_point;
static bool s_gesture_armed;

static bool rotate_pointer_down(void)
{
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return true;
        if (lv_indev_get_scroll_obj(indev) == s_tileview) return true;
    }
    return false;
}

/* The release handler runs before LVGL's fling. Drop the predicted distance
 * so that fling cannot add a second page on top of the one we commit. */
static void rotate_kill_fling(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    indev->pointer.scroll_throw_vect.x = 0;
    indev->pointer.scroll_throw_vect.y = 0;
    indev->pointer.scroll_throw_vect_ori.x = 0;
    indev->pointer.scroll_throw_vect_ori.y = 0;
}

/* Finger is up. Keep the page the finger actually dragged toward, and do not
 * let the fling travel any further than that one page. */
static void rotate_finish_gesture(void)
{
    rotate_kill_fling();
    if (!s_gesture_armed || !s_tileview) return;
    s_gesture_armed = false;

    int32_t w = lv_obj_get_content_width(s_tileview);
    if (w <= 8) return;

    lv_point_t point = s_gesture_point;
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &point);
    int32_t finger_dx = point.x - s_gesture_point.x;
    int32_t finger_dy = point.y - s_gesture_point.y;
    if (finger_dy < 0) finger_dy = -finger_dy;

    int32_t scroll_dx = lv_obj_get_scroll_x(s_tileview) - s_gesture_x;
    int start = (int)((s_gesture_x + (w / 2)) / w);
    if (start < 0) start = 0;
    if (start >= PAGE_COUNT) start = PAGE_COUNT - 1;

    /* +1 follows the finger toward network. -1 follows it back toward settings. */
    int dir = 0;
    if (scroll_dx > w / 6) dir = 1;
    else if (scroll_dx < -(w / 6)) dir = -1;
    else if (start == PAGE_COUNT - 1 && finger_dx < -(w / 6) && -finger_dx > finger_dy) {
        /* The strip cannot scroll past network, so this swipe never moves
         * scroll_x. It is the same step the auto cycle takes: network to clock. */
        dir = 1;
    } else if (start == 0 && finger_dx > w / 6 && finger_dx > finger_dy) {
        /* The strip cannot scroll before settings. A right swipe wraps to network. */
        dir = -1;
    }

    int target = start + dir;
    if (target >= PAGE_COUNT) target = 1;
    if (target < 0) target = PAGE_COUNT - 1;
    if (target == start && scroll_dx > -8 && scroll_dx < 8) return;

    lv_obj_stop_scroll_anim(s_tileview);
    lv_tileview_set_tile_by_index(s_tileview, (uint32_t)target, 0, LV_ANIM_OFF);
}

/* Does not stop or rewind a scroll while the finger is down. */
static void rotate_indev_cb(lv_event_t *e)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_finger = true;
        s_rotate_ms = now;
        page_dots_poke(now);
        if (s_tileview) {
            s_gesture_x = lv_obj_get_scroll_x(s_tileview);
            s_gesture_point.x = 0;
            s_gesture_point.y = 0;
            if (lv_indev_active()) lv_indev_get_point(lv_indev_active(), &s_gesture_point);
            s_gesture_armed = true;
        }
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        s_finger = false;
        s_rotate_ms = now;
        rotate_finish_gesture();
    }
}

static void rotate_bind_indev(void)
{
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        /* Default fling predicts several screens. One sample of velocity
         * cannot skip a page after the gesture is committed. */
        lv_indev_set_scroll_throw(indev, 100);
        lv_indev_add_event_cb(indev, rotate_indev_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, rotate_indev_cb, LV_EVENT_RELEASED, NULL);
    }
}

/* Column the strip has actually stopped on, or -1 while it is still between pages. */
static int rotate_settled_page(void)
{
    int32_t w = lv_obj_get_content_width(s_tileview);
    if (w <= 0) return -1;
    int32_t x = lv_obj_get_scroll_x(s_tileview);
    int32_t page = (x + (w / 2)) / w;
    if (page < 0 || page >= PAGE_COUNT) return -1;
    int32_t dx = x - page * w;
    if (dx < 0) dx = -dx;
    if (dx > 8) return -1;
    return (int)page;
}

/* Content pages only: clock, weather, CPU, GPU, RAM, network. Settings stays put.
 * The change is instant and only runs while the finger is up and the strip
 * has stopped. A touch or a swipe restarts the countdown from the page that
 * is actually on screen, even if a scroll-end event was skipped. */
static void rotate_poll(uint32_t now)
{
    if (!s_tileview || s_blanked) {
        s_rotate_ms = now;
        return;
    }
    if (s_finger || rotate_pointer_down() || lv_obj_is_scrolling(s_tileview)) {
        s_rotate_ms = now;
        return;
    }

    int page = rotate_settled_page();
    if (page < 0) {
        s_rotate_ms = now;
        return;
    }
    if (page != s_page_index) {
        page_dots_sync(page);
        s_rotate_ms = now;
        return;
    }
    if (s_rotate_s == 0 || page <= 0) {
        s_rotate_ms = now;
        return;
    }
    if (s_rotate_ms == 0) s_rotate_ms = now;
    if ((uint32_t)(now - s_rotate_ms) < (uint32_t)s_rotate_s * 1000u) return;

    int next = page + 1;
    if (next >= PAGE_COUNT) next = 1;
    s_rotate_ms = now;
    lv_tileview_set_tile_by_index(s_tileview, (uint32_t)next, 0, LV_ANIM_OFF);
}

static uint8_t brightness_load(void)
{
    FILE *f = fopen(BRIGHTNESS_PATH, "rb");
    if (!f) return 100;
    int c = fgetc(f);
    fclose(f);
    if (c < 10 || c > 100) return 100;
    return (uint8_t)c;
}

static void brightness_save(uint8_t percent)
{
    FILE *f = fopen(BRIGHTNESS_PATH, "wb");
    if (!f) return;
    fputc(percent, f);
    fclose(f);
}

static lv_obj_t *s_bright_value;
static lv_obj_t *s_batt_icon;
static lv_obj_t *s_batt_bolt;
static lv_obj_t *s_batt_text;
static lv_obj_t *s_link_usb;
static lv_obj_t *s_link_ble;

static void bright_value_set(int percent)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    if (s_bright_value) lv_label_set_text(s_bright_value, buf);
}

static void bright_arc_cb(lv_event_t *e)
{
    int value = lv_arc_get_value(lv_event_get_target(e));
    display_board_set_brightness((uint8_t)value);
    bright_value_set(value);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        brightness_save((uint8_t)value);
    }
}

static void rotate_arc_cb(lv_event_t *e)
{
    int pos = lv_arc_get_value(lv_event_get_target(e));
    uint16_t seconds = rotate_normalize(pos <= 0 ? 0 : pos * ROTATE_STEP_S);
    s_rotate_s = seconds;
    s_rotate_ms = (uint32_t)(esp_timer_get_time() / 1000);
    rotate_value_set(seconds);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) rotate_save(seconds);
}

static lv_obj_t *settings_caption(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x90CAF9), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

/* Upper or lower semicircle. The widget is the full 466 circle, and LVGL
 * draws the stroke out to that radius, so the arcs sit on the glass edge.
 * Only the stroke is hit-tested. */
static lv_obj_t *settings_arc(lv_obj_t *parent, int start, int end,
                              int min, int max, int value, uint32_t color, bool reverse)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 466, 466);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_range(arc, min, max);
    lv_arc_set_mode(arc, reverse ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
    lv_arc_set_value(arc, value);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_ADV_HITTEST | LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_set_style_arc_width(arc, 32, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 32, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x55555C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    return arc;
}

static lv_obj_t *settings_row(lv_obj_t *parent, int y, int width, int gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, width, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void settings_refresh(const power_batt_t *batt, int transport, bool fresh)
{
    if (!s_batt_icon || !batt) return;
    static int last_pct = -2;
    static int last_flags = -1;
    int flags = (batt->present ? 1 : 0) | (batt->charging ? 2 : 0) |
                (fresh ? 4 : 0) | (transport << 4);
    if (last_pct == batt->percent && last_flags == flags) return;
    last_pct = batt->percent;
    last_flags = flags;

    const char *icon = LV_SYMBOL_BATTERY_EMPTY;
    uint32_t color = 0x7D868F;
    char text[8] = "--";
    if (batt->present && batt->percent >= 0) {
        int pct = batt->percent;
        if (pct > 100) pct = 100;
        if (pct >= 80) icon = LV_SYMBOL_BATTERY_FULL;
        else if (pct >= 60) icon = LV_SYMBOL_BATTERY_3;
        else if (pct >= 40) icon = LV_SYMBOL_BATTERY_2;
        else if (pct >= 15) icon = LV_SYMBOL_BATTERY_1;
        color = batt->charging ? 0xFFD54F : (pct <= 15 ? 0xEF5350 : 0xF2F5F8);
        snprintf(text, sizeof(text), "%d%%", pct);
    }

    lv_label_set_text(s_batt_icon, icon);
    lv_obj_set_style_text_color(s_batt_icon, lv_color_hex(color), 0);
    lv_label_set_text(s_batt_bolt, batt->charging ? LV_SYMBOL_CHARGE : "");
    lv_obj_set_style_text_color(s_batt_bolt, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_batt_text, text);
    lv_obj_set_style_text_color(s_batt_text, lv_color_hex(color), 0);

    uint32_t usb_color = 0x7D868F;
    uint32_t ble_color = 0x7D868F;
    if (fresh && transport == 1) usb_color = 0xFFFFFF;
    if (fresh && transport == 2) ble_color = 0xFFFFFF;
    lv_obj_set_style_text_color(s_link_usb, lv_color_hex(usb_color), 0);
    lv_obj_set_style_text_color(s_link_ble, lv_color_hex(ble_color), 0);
}

static lv_obj_t *settings_value(lv_obj_t *parent, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    screen_pass_swipe(label);
    return label;
}

static void create_settings_page(lv_obj_t *tile)
{
    lv_obj_t *panel = screen_panel_create(tile);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

    /* 0° is 3 o'clock, clockwise. 188→352 is the top half, 8→172 the bottom,
     * with a gap so the rounded ends do not meet. */
    lv_obj_t *bright = settings_arc(panel, 188, 352, 10, 100,
                                    display_board_get_brightness(), 0xF2F5F8, false);
    lv_obj_add_event_cb(bright, bright_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bright, bright_arc_cb, LV_EVENT_RELEASED, NULL);

    int rotate_pos = s_rotate_s == 0 ? 0 : s_rotate_s / ROTATE_STEP_S;
    lv_obj_t *rotate = settings_arc(panel, 8, 172, 0, ROTATE_MAX_S / ROTATE_STEP_S,
                                    rotate_pos, 0x0071C5, true);
    lv_obj_add_event_cb(rotate, rotate_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(rotate, rotate_arc_cb, LV_EVENT_RELEASED, NULL);

    screen_pass_swipe(settings_caption(panel, "Brightness", -108));
    s_bright_value = settings_value(panel, -70);
    bright_value_set(display_board_get_brightness());

    lv_obj_t *status = settings_row(panel, 8, 320, 28);
    s_link_usb = lv_label_create(status);
    lv_obj_set_style_text_font(s_link_usb, &lv_font_montserrat_26, 0);
    lv_label_set_text(s_link_usb, LV_SYMBOL_USB);
    lv_obj_set_style_text_color(s_link_usb, lv_color_hex(0x7D868F), 0);
    s_link_ble = lv_label_create(status);
    lv_obj_set_style_text_font(s_link_ble, &lv_font_montserrat_26, 0);
    lv_label_set_text(s_link_ble, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_link_ble, lv_color_hex(0x7D868F), 0);

    lv_obj_t *batt = lv_obj_create(status);
    lv_obj_set_size(batt, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(batt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt, 0, 0);
    lv_obj_set_style_pad_all(batt, 0, 0);
    lv_obj_set_style_pad_column(batt, 6, 0);
    lv_obj_set_flex_flow(batt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(batt, LV_OBJ_FLAG_SCROLLABLE);
    s_batt_icon = lv_label_create(batt);
    lv_obj_set_style_text_font(s_batt_icon, &lv_font_montserrat_26, 0);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(s_batt_icon, lv_color_hex(0x7D868F), 0);
    s_batt_bolt = lv_label_create(batt);
    lv_obj_set_style_text_font(s_batt_bolt, &lv_font_montserrat_22, 0);
    s_batt_text = lv_label_create(batt);
    lv_obj_set_style_text_font(s_batt_text, &lv_font_montserrat_26, 0);
    lv_label_set_text(s_batt_text, "--");
    lv_obj_set_style_text_color(s_batt_text, lv_color_hex(0x7D868F), 0);
    screen_pass_swipe(status);

    power_batt_t none = {0};
    none.percent = -1;
    settings_refresh(&none, 0, false);

    screen_pass_swipe(settings_caption(panel, "Switch", 68));
    s_rotate_value = settings_value(panel, 104);
    rotate_value_set(s_rotate_s);
}

/* BOOT is GPIO0, active low. A short press blanks the panel, or wakes it. */
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int last = 1;
    int stable = 0;
    while (1) {
        int level = gpio_get_level(BOOT_GPIO);
        if (level == last) {
            stable++;
        } else {
            last = level;
            stable = 0;
        }
        if (stable == 2 && level == 0) {
            /* lv_refr_now does not fit on this task. The display task applies it. */
            s_panel_req = 1;
            vTaskDelay(pdMS_TO_TICKS(350));
            last = gpio_get_level(BOOT_GPIO);
            stable = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* =============================================================================
 * SCREENSAVER THEME COLORS (from gui_settings)
 * ========================================================================== */
#define COLOR_SONIC_BG      lv_color_hex(gui_settings.ss_bg_color[SCREEN_CPU])
#define COLOR_ART_BG        lv_color_hex(gui_settings.ss_bg_color[SCREEN_GPU])
#define COLOR_DK_BG         lv_color_hex(gui_settings.ss_bg_color[SCREEN_RAM])
#define COLOR_PACMAN_BG     lv_color_hex(gui_settings.ss_bg_color[SCREEN_NET])

/* =============================================================================
 * THEME UPDATE CALLBACK (Thread-Safe)
 *
 * Called by gui_settings when SET_SS_BG command is received.
 * Acquires LVGL mutex and applies theme changes.
 * ========================================================================== */
static void theme_update_callback(void)
{
    if (ui_acquire_lock(LVGL_MUTEX_TIMEOUT_MS)) {
        ui_manager_apply_theme();
        ui_release_lock();
        ESP_LOGI(TAG, "Theme updated via SET_SS_BG command");
    } else {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for theme update");
    }
}

/* =============================================================================
 * TASK: Display Update - 10 FPS with Screensaver Logic
 * ========================================================================== */
static void display_update_task(void *arg)
{
    ESP_LOGI(TAG, "Display Update Task started (10 FPS)");

    /* Subscribe to Task Watchdog */
    esp_task_wdt_add(NULL);

    while (1) {
        /* Feed the watchdog at start of each iteration */
        esp_task_wdt_reset();

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        static power_batt_t s_power;
        static uint32_t s_power_ms;
        if ((uint32_t)(now - s_power_ms) >= 1000) {
            s_power_ms = now;
            power_batt_t sample;
            if (power_axp_read(&sample)) s_power = sample;
        }
        uint32_t last_data = usb_serial_get_last_data_time();
        bool never_had_data = (last_data == 0);
        uint32_t time_since_data = never_had_data ? UINT32_MAX : (now - last_data);

        bool data_is_stale = (time_since_data > STALE_DATA_THRESHOLD_MS);
        bool no_data = (time_since_data > NO_DATA_BLANK_MS);

        /* Same lock the BSP LVGL task uses. Never wait forever. */
        if (ui_acquire_lock(LVGL_MUTEX_TIMEOUT_MS)) {

            /* Process pending image reloads from USB task (Thread-Safety Fix)
             * This MUST be done in the UI thread to avoid race conditions */
            ss_process_updates();

            /* Apply new hardware names (NAME_CPU/NAME_GPU) in the UI thread */
            if (hw_identity_consume_names_dirty()) {
                ui_manager_apply_hardware_names();
            }

            int panel_req = s_panel_req;
            if (panel_req) {
                s_panel_req = 0;
                if (panel_req == 2 || s_blanked) wake_screen();
                else blank_screen(true);
            }

            /* A manual blank stays off while the computer screen stays on.
             * Only a real off-to-on change turns the panel back on, and it
             * does not override a blank the user asked for with the button. */
            int pc_panel = s_pc_panel;
            if (pc_panel != s_pc_panel_applied && (pc_panel == 0 || pc_panel == 1)) {
                int prev = s_pc_panel_applied;
                s_pc_panel_applied = pc_panel;
                if (pc_panel == 0) {
                    if (!s_blank_manual) blank_screen(false);
                } else if (prev == 0 && !s_blank_manual) {
                    wake_screen();
                }
            }

            /* No data: leave the page up for 10 seconds, then turn the panel off.
             * A tap or BOOT wakes it and asks for data again. Data that arrives
             * while the panel is off does not turn it back on by itself.
             * A manual BOOT blank stays dark until the next tap or press. */
            uint32_t since_wake = now - s_wake_ms;
            /* wake_screen() stamps a time after `now` was sampled. The
             * unsigned subtract would look like the panel had been awake
             * for weeks, so a tap would turn it straight back off. */
            if ((int32_t)(now - s_wake_ms) < 0) since_wake = 0;
            if (ble_link_wants_ui() && s_blanked && !s_blank_manual) wake_screen();
            ble_link_ui_poll(now);
            settings_refresh(&s_power, usb_serial_transport(),
                             !never_had_data && !data_is_stale);

            /* USB and Bluetooth handoff stops samples for a moment. When
             * they resume, turn a no-data blank back on. A button blank
             * and a dark computer monitor stay off. */
            if (s_blanked && !s_blank_manual && s_pc_panel != 0 &&
                !never_had_data && !data_is_stale) {
                wake_screen();
            }

            if (!s_blank_manual && !s_blanked && no_data && since_wake >= NO_DATA_BLANK_MS &&
                !ble_link_wants_ui()) {
                blank_screen(false);
            }

            /* A swipe already redraws the whole page. Updating gauges at the
             * same time restarts that draw and the slide tears and stalls. */
            bool swiping = s_tileview && lv_obj_is_scrolling(s_tileview);
            if (!swiping) burn_in_shift_poll(now);
            rotate_poll(now);
            page_dots_poll(now);

            /* Red dot logic */
            if (!swiping) {
                if (data_is_stale && !s_blanked) {
                    ui_manager_show_status_dots(true);
                } else {
                    ui_manager_show_status_dots(false);
                }
            }

            /* Update screens (only while the panel is awake and still) */
            if (!s_blanked && !swiping) {
                /* Acquire stats mutex with timeout - NEVER use portMAX_DELAY! */
                if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    pc_stats_t local_stats = *usb_serial_get_stats();
                    xSemaphoreGive(s_stats_mutex);

                    ui_manager_update_screens(&local_stats);
                } else {
                    /* Fail-safe: Skip this frame, don't freeze! */
                    ESP_LOGW(TAG, "Stats mutex timeout in display task - skipping frame");
                }
            }

            ui_release_lock();
        } else {
            /* Fail-safe: LVGL lock timeout - skip this frame */
            ESP_LOGW(TAG, "LVGL lock timeout in display task - skipping frame");
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

/* =============================================================================
 * LVGL heap: prefer PSRAM, fall back to internal RAM
 * ========================================================================== */
void *lv_custom_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

void lv_custom_free(void *ptr)
{
    heap_caps_free(ptr);
}

void *lv_custom_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && size > 0) {
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return p;
}

/* =============================================================================
 * MAIN APPLICATION ENTRY
 * ========================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "PC Monitor - Waveshare AMOLED 1.75C v2.5");
    ESP_LOGI(TAG, "===========================================");

    /* Initialize Task Watchdog Timer (TWDT)
     * If a task hangs for > TWDT_TIMEOUT_SEC, the ESP32 will reset automatically.
     * This prevents permanent freezes - 24/7 stability! */
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = TWDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,                    /* Don't watch idle tasks */
        .trigger_panic = true                   /* Hard reset on timeout */
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&twdt_config));
    ESP_LOGI(TAG, "Task Watchdog configured: %d sec timeout, panic on freeze", TWDT_TIMEOUT_SEC);

    /* Initialize LittleFS storage */
    if (storage_init() == ESP_OK) {
        hw_identity_load();
        gui_settings_load();
        display_board_set_brightness(brightness_load());
        s_rotate_s = rotate_load();
    } else {
        gui_settings_init_defaults(&gui_settings);
    }

    /* Stats mutex. LVGL locking goes through the BSP adapter. */
    s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create stats mutex!");
        return;
    }

    /* Initialize USB Serial and start reading before the display comes up.
     * Opening the port resets the board; the plugin's handshake has to be
     * answered while the panel is still initialising. */
    usb_serial_init();

    /* Register command handlers */
    usb_serial_register_handler(handle_pc_screen_cmd);
    usb_serial_register_handler(hw_identity_handle_command);
    usb_serial_register_handler(ui_manager_handle_color_command);
    usb_serial_register_handler(ss_image_handle_command);
    usb_serial_register_handler(gui_settings_handle_command);
    usb_serial_register_handler(fw_update_handle_command);
    usb_serial_start_rx_task(s_stats_mutex);
    ble_link_start();

    /* Set theme callback for gui_settings (SET_SS_BG command) */
    gui_settings_set_theme_callback(theme_update_callback);

    /* Initialize UI manager */
    ui_manager_init(NULL);

    /* Same CO5300 path as moto-gps-waveshare: hidden until the first frame. */
    lv_display_t *disp = display_board_init();
    if (!disp) {
        ESP_LOGE(TAG, "Waveshare display init failed");
        return;
    }
    ESP_LOGI(TAG, "AMOLED ready: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);

    if (!ui_acquire_lock(5000)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock during init!");
        return;
    }

    ss_images_init();

    hw_identity_t *hw_id = hw_identity_get();

    s_tileview = lv_tileview_create(NULL);
    /* SCROLL_ONE's limit turns the backward swipe into a forward one, so the
     * network page cannot be dragged back toward weather. Momentum was
     * landing two pages past the finger. */
    lv_obj_remove_flag(s_tileview, LV_OBJ_FLAG_SCROLL_ONE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(s_tileview, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tileview, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    rotate_bind_indev();
    lv_screen_load(s_tileview);

    /* Both directions on every page. A one-way lock on the end page was
     * swallowing the swipe that leads back into the list. */
    lv_obj_t *page_set = add_page(0, LV_DIR_HOR);
    lv_obj_t *page_clk = add_page(1, LV_DIR_HOR);
    lv_obj_t *page_wx = add_page(2, LV_DIR_HOR);
    lv_obj_t *page_cpu = add_page(3, LV_DIR_HOR);
    lv_obj_t *page_gpu = add_page(4, LV_DIR_HOR);
    lv_obj_t *page_ram = add_page(5, LV_DIR_HOR);
    lv_obj_t *page_net = add_page(6, LV_DIR_HOR);
    create_settings_page(page_set);
    s_screens.clock = screen_clock_create(page_clk);
    if (s_screens.clock && s_screens.clock->screen) {
        s_dots.clock = ui_manager_create_status_dot(s_screens.clock->screen);
        /* Upper right, clear of the 12. Same corner as the sensor on the product. */
        if (s_dots.clock) lv_obj_align(s_dots.clock, LV_ALIGN_CENTER, 148, -128);
    }
    s_screens.weather = screen_weather_create(page_wx);
    if (s_screens.weather && s_screens.weather->screen) {
        s_dots.weather = ui_manager_create_status_dot(s_screens.weather->screen);
    }

    s_screens.cpu = screen_cpu_create(page_cpu);
    if (s_screens.cpu && s_screens.cpu->screen) {
        if (s_screens.cpu->label_title) {
            lv_label_set_text(s_screens.cpu->label_title, hw_id->cpu_name);
        }
        s_dots.cpu = ui_manager_create_status_dot(s_screens.cpu->screen);
        s_screensavers.cpu = ui_manager_create_screensaver_ex(
            s_screens.cpu->screen, COLOR_SONIC_BG, ss_image_get_dsc(SS_IMG_CPU), SS_IMG_CPU);
    }

    s_screens.gpu = screen_gpu_create(page_gpu);
    if (s_screens.gpu && s_screens.gpu->screen) {
        if (s_screens.gpu->label_title) {
            lv_label_set_text(s_screens.gpu->label_title, hw_id->gpu_name);
        }
        s_dots.gpu = ui_manager_create_status_dot(s_screens.gpu->screen);
        s_screensavers.gpu = ui_manager_create_screensaver_ex(
            s_screens.gpu->screen, COLOR_ART_BG, ss_image_get_dsc(SS_IMG_GPU), SS_IMG_GPU);
    }

    s_screens.ram = screen_ram_create(page_ram);
    if (s_screens.ram && s_screens.ram->screen) {
        s_dots.ram = ui_manager_create_status_dot(s_screens.ram->screen);
        s_screensavers.ram = ui_manager_create_screensaver_ex(
            s_screens.ram->screen, COLOR_DK_BG, ss_image_get_dsc(SS_IMG_RAM), SS_IMG_RAM);
    }

    s_screens.network = screen_network_create(page_net);
    if (s_screens.network && s_screens.network->screen) {
        s_dots.net = ui_manager_create_status_dot(s_screens.network->screen);
        s_screensavers.net = ui_manager_create_screensaver_ex(
            s_screens.network->screen, COLOR_PACMAN_BG, ss_image_get_dsc(SS_IMG_NET), SS_IMG_NET);
    }

    /* Register UI handles with manager */
    ui_manager_set_screens(&s_screens);
    ui_manager_set_screensavers(&s_screensavers);
    ui_manager_set_status_dots(&s_dots);

    /* Register callback for screensaver image hot-swap (Thread-Safety Fix) */
    ss_set_reload_callback((ss_image_reload_cb_t)ui_manager_on_image_reload);

    screen_pass_swipe(s_screens.clock ? s_screens.clock->screen : NULL);
    screen_pass_swipe(s_screens.weather ? s_screens.weather->screen : NULL);
    screen_pass_swipe(s_screens.cpu ? s_screens.cpu->screen : NULL);
    screen_pass_swipe(s_screens.gpu ? s_screens.gpu->screen : NULL);
    screen_pass_swipe(s_screens.ram ? s_screens.ram->screen : NULL);
    screen_pass_swipe(s_screens.network ? s_screens.network->screen : NULL);

    lv_obj_t *top = lv_layer_top();
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_page_dot[i] = lv_obj_create(top);
        lv_obj_set_size(s_page_dot[i], 10, 10);
        lv_obj_set_style_radius(s_page_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_page_dot[i], lv_color_white(), 0);
        lv_obj_set_style_border_width(s_page_dot[i], 0, 0);
        lv_obj_set_style_pad_all(s_page_dot[i], 0, 0);
        lv_obj_remove_flag(s_page_dot[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_page_dot[i], LV_ALIGN_BOTTOM_MID,
                     (i * 22) - (((PAGE_COUNT - 1) * 22) / 2), -46);
    }
    /* Settings stays the leftmost page and is not in the auto cycle.
     * Auto cycle boots on the clock. With switching off, resume the last page. */
    int boot_page = (s_rotate_s == 0) ? page_load() : PAGE_CLOCK;
    lv_tileview_set_tile_by_index(s_tileview, (uint32_t)boot_page, 0, LV_ANIM_OFF);
    page_dots_sync(boot_page);

    s_blank_cover = lv_obj_create(top);
    lv_obj_set_size(s_blank_cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_blank_cover, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_blank_cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_blank_cover, 0, 0);
    lv_obj_set_style_radius(s_blank_cover, 0, 0);
    lv_obj_set_style_pad_all(s_blank_cover, 0, 0);
    lv_obj_remove_flag(s_blank_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_blank_cover, blank_cover_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_blank_cover, LV_OBJ_FLAG_HIDDEN);
    s_blanked = false;
    s_wake_ms = (uint32_t)(esp_timer_get_time() / 1000);
    lv_refr_now(lv_display_get_default());
    display_board_set_awake(true);
    ui_release_lock();
    ESP_LOGI(TAG, "Page stays up for %d ms without data, then the panel turns off", NO_DATA_BLANK_MS);

    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL);

    xTaskCreatePinnedToCore(display_update_task, "disp_upd", STACK_SIZE_DISPLAY_UPD, NULL, PRIO_DISPLAY_UPDATE, NULL, 0);

    ESP_LOGI(TAG, "Task stack sizes: USB_RX=%d, Display=%d",
             STACK_SIZE_USB_RX, STACK_SIZE_DISPLAY_UPD);
    ESP_LOGI(TAG, "Task priorities: USB_RX=%d, Display=%d",
             PRIO_USB_RX, PRIO_DISPLAY_UPDATE);

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "System ready. Screen turns off after 10 seconds without data.");
    ESP_LOGI(TAG, "===========================================");
}
