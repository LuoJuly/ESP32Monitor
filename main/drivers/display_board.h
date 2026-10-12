/**
 * CO5300 bring-up for the Waveshare 1.75C.
 * Partial frames in PSRAM, copied to internal RAM for DMA.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** Panel stays dark until display_board_reveal(). */
lv_display_t *display_board_init(void);

/** Turn the panel on after the first frame is queued, then arm TE sync. */
esp_err_t display_board_reveal(void);

/** AMOLED brightness: awake uses the saved level, asleep turns the panel off. Touch stays active. */
esp_err_t display_board_set_awake(bool awake);

/** 10 to 100. Applied immediately while the panel is on, and again on the next wake. */
void display_board_set_brightness(uint8_t percent);
uint8_t display_board_get_brightness(void);
