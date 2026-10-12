/**
 * Bluetooth LE link for the same text lines the USB port already carries.
 * The first time a computer connects, the screen asks for a tap. After that
 * the computer's token is remembered and later connections open on their own.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

void ble_link_start(void);

/** Caller holds the LVGL lock. */
void ble_link_ui_poll(uint32_t now_ms);

bool ble_link_wants_ui(void);
