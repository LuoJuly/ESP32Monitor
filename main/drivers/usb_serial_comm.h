/**
 * @file usb_serial_comm.h
 * @brief USB Serial Communication Driver
 *
 * Handles USB Serial JTAG communication, line buffering, and command dispatch.
 */

#ifndef USB_SERIAL_COMM_H
#define USB_SERIAL_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/system_types.h"

/* Configuration */
#define USB_LINE_BUFFER_SIZE    4096    /* Max line length (larger for IMG_DATA chunks) */
#define USB_RX_BUFFER_SIZE      4096    /* Must be >= max line length to prevent data loss */
#define USB_TX_BUFFER_SIZE      1024

/* Command handler callback type */
typedef bool (*usb_cmd_handler_t)(const char *line);

/**
 * @brief Initialize USB Serial JTAG driver
 * @return ESP_OK on success
 */
esp_err_t usb_serial_init(void);

/**
 * @brief Start USB RX task
 * @param stats_mutex Mutex for protecting pc_stats updates
 */
void usb_serial_start_rx_task(SemaphoreHandle_t stats_mutex);

/**
 * @brief Get pointer to current PC stats
 * @return Pointer to pc_stats_t structure
 */
pc_stats_t *usb_serial_get_stats(void);

/**
 * @brief Get timestamp of last received data (ms since boot)
 * @return Timestamp in milliseconds
 */
uint32_t usb_serial_get_last_data_time(void);

/**
 * @brief Register a command handler
 *
 * Handlers are called in registration order until one returns true.
 *
 * @param handler Function that returns true if it handled the command
 */
void usb_serial_register_handler(usb_cmd_handler_t handler);

/**
 * @brief Send response string via USB Serial
 * @param response String to send (will be sent as-is)
 */
/**
 * @brief Dispatch one complete line (no newline). USB and Bluetooth both use this.
 */
void usb_serial_handle_line(const char *line);

/**
 * Same as usb_serial_handle_line. from_ble is remembered when a sample
 * is accepted, so the settings page can show which link is carrying data.
 */
void usb_serial_handle_line_from(const char *line, bool from_ble);

/** 0 none, 1 USB, 2 Bluetooth. */
int usb_serial_transport(void);

/**
 * @brief Extra transmitter. Bluetooth notifications register here so replies
 * leave on whichever link is up.
 */
typedef void (*usb_serial_tx_fn)(const char *data, size_t len);
void usb_serial_set_tx_fn(usb_serial_tx_fn fn);

void usb_serial_send(const char *response);

/**
 * @brief Send formatted response via USB Serial
 * @param fmt Printf-style format string
 * @param ... Format arguments
 */
void usb_serial_sendf(const char *fmt, ...);

#endif /* USB_SERIAL_COMM_H */
