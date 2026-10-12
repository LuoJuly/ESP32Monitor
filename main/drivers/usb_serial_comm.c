/**
 * @file usb_serial_comm.c
 * @brief USB Serial Communication Driver Implementation
 */

#include "usb_serial_comm.h"
#include "../storage/hw_identity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_app_desc.h"

static const char *TAG = "USB-COMM";

/* Thread-safety configuration */
#define STATS_MUTEX_TIMEOUT_MS  100  /* Never block indefinitely! */

/* N/A hold ("glitch absorption"):
 * The PC client sends -1 for a sensor when a single read fails (e.g.
 * LibreHardwareMonitor returns null for one update cycle). Showing that
 * instantly makes the screen flick to "N/A" and back. Instead we keep the
 * last valid value for up to HOLD_MAX_PACKETS consecutive misses; only a
 * sustained outage (sensor really gone) is shown as N/A. */
#define HOLD_MAX_PACKETS  4

/* Routine resync is once an hour. CLK is network civil time at the
 * weather fix. The 40 MHz crystal runs the dial between syncs; at
 * 20 ppm an hour is about 70 ms, inside one second. The stamp has no
 * milliseconds, so a one-second disagreement is just sampling. Two
 * seconds means the source actually moved, and that is applied at once. */
#define CLK_RESYNC_MS        (3600UL * 1000UL)
#define CLK_STEP_SEC         2

/* Global state */
static pc_stats_t s_pc_stats = {
    .clock_sec = -1,
};
static volatile uint32_t s_last_data_ms = 0;
static bool s_rx_is_ble;
static volatile int s_transport;
static SemaphoreHandle_t s_stats_mutex = NULL;
static uint32_t s_stats_mutex_timeouts = 0;

/* Per-field miss counters for the N/A hold logic */
static struct {
    uint8_t cpu_pct, cpu_temp, gpu_pct, gpu_temp, vram, ram, net_dn, net_up;
} s_hold = {0};

/* True when this CLK stamp should replace the crystal time base. */
static bool clock_accept_sync(int32_t pc_sec)
{
    if (s_pc_stats.clock_sec < 0) return true;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t elapsed = now - s_pc_stats.clock_sync_ms;
    if (elapsed >= CLK_RESYNC_MS) return true;

    int32_t crystal = s_pc_stats.clock_sec + (int32_t)(elapsed / 1000);
    crystal %= 86400;
    if (crystal < 0) crystal += 86400;
    int32_t diff = pc_sec - crystal;
    if (diff > 43200) diff -= 86400;
    if (diff < -43200) diff += 86400;
    if (diff < 0) diff = -diff;
    return diff >= CLK_STEP_SEC;
}

static void clock_keep_last(pc_stats_t *dst)
{
    dst->clock_sec = s_pc_stats.clock_sec;
    dst->clock_sync_ms = s_pc_stats.clock_sync_ms;
    dst->clock_year = s_pc_stats.clock_year;
    dst->clock_mon = s_pc_stats.clock_mon;
    dst->clock_day = s_pc_stats.clock_day;
}

/* If the new field is N/A (<0) and we haven't held too long, keep the old
 * value; otherwise accept the new value and reset the counter. */
#define HOLD_FIELD(newv, oldv, counter) do {          \
    if ((newv) < 0) {                                 \
        if ((counter) < HOLD_MAX_PACKETS) {           \
            (newv) = (oldv);                          \
            (counter)++;                              \
        }                                             \
    } else {                                          \
        (counter) = 0;                                \
    }                                                 \
} while (0)

/* Command handlers (max 8) */
#define MAX_CMD_HANDLERS 8
static usb_cmd_handler_t s_handlers[MAX_CMD_HANDLERS] = {0};
static int s_handler_count = 0;

/* =============================================================================
 * INITIALIZATION
 * ========================================================================== */

esp_err_t usb_serial_init(void)
{
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = USB_RX_BUFFER_SIZE,
        .tx_buffer_size = USB_TX_BUFFER_SIZE
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&usb_cfg);
    /* The USB console on this board may already own the driver. */
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    s_pc_stats.weather_temp = -999;
    s_pc_stats.weather_code = -1;
    s_pc_stats.weather_hum = -1;
    s_pc_stats.sun_rise_min = -1;
    s_pc_stats.sun_set_min = -1;

    if (ret == ESP_OK) {
        /* Console output otherwise touches the FIFO directly and drops host bytes. */
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(TAG, "USB Serial JTAG initialized");
    } else {
        ESP_LOGE(TAG, "USB Serial JTAG init failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* =============================================================================
 * ACCESSORS
 * ========================================================================== */

pc_stats_t *usb_serial_get_stats(void)
{
    return &s_pc_stats;
}

uint32_t usb_serial_get_last_data_time(void)
{
    return s_last_data_ms;
}

void usb_serial_register_handler(usb_cmd_handler_t handler)
{
    if (s_handler_count < MAX_CMD_HANDLERS && handler != NULL) {
        s_handlers[s_handler_count++] = handler;
    }
}

/* =============================================================================
 * SEND FUNCTIONS
 * ========================================================================== */

static usb_serial_tx_fn s_tx_fn;

void usb_serial_set_tx_fn(usb_serial_tx_fn fn)
{
    s_tx_fn = fn;
}

void usb_serial_send(const char *response)
{
    if (response) {
        size_t len = strlen(response);
        usb_serial_jtag_write_bytes((const uint8_t *)response, len, pdMS_TO_TICKS(100));
        if (s_tx_fn && len) s_tx_fn(response, len);
    }
}

void usb_serial_sendf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        usb_serial_jtag_write_bytes((const uint8_t *)buf, len, pdMS_TO_TICKS(100));
        if (s_tx_fn) s_tx_fn(buf, (size_t)len);
    }
}

/* =============================================================================
 * DATA PARSER
 * ========================================================================== */

static void parse_pc_data(const char *line)
{
    if (!line || strlen(line) < 5) return;

    /* Static: this 4KB buffer does not fit on the RX task stack. */
    static char buffer[USB_LINE_BUFFER_SIZE];
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    /* Parse into temporary struct first to avoid partial updates */
    pc_stats_t temp_stats = {0};
    temp_stats.weather_temp = -999;
    temp_stats.weather_code = -1;
    temp_stats.weather_hum = -1;
    temp_stats.sun_rise_min = -1;
    temp_stats.sun_set_min = -1;
    temp_stats.clock_sec = -1;
    int fields_parsed = 0;
    bool saw_weather = false;
    bool saw_sun = false;
    bool saw_clock = false;
    char *token = strtok(buffer, ",");

    while (token != NULL) {
        if (strncmp(token, "CPU:", 4) == 0) {
            temp_stats.cpu_percent = (int16_t)atoi(token + 4);
            fields_parsed++;
        }
        else if (strncmp(token, "CPUT:", 5) == 0) {
            temp_stats.cpu_temp = atof(token + 5);
            fields_parsed++;
        }
        else if (strncmp(token, "GPU:", 4) == 0) {
            temp_stats.gpu_percent = (int16_t)atoi(token + 4);
            fields_parsed++;
        }
        else if (strncmp(token, "GPUT:", 5) == 0) {
            temp_stats.gpu_temp = atof(token + 5);
            fields_parsed++;
        }
        else if (strncmp(token, "VRAM:", 5) == 0) {
            if (sscanf(token + 5, "%f/%f", &temp_stats.gpu_vram_used, &temp_stats.gpu_vram_total) == 2) {
                fields_parsed++;
            } else {
                temp_stats.gpu_vram_used = -1.0f;   /* N/A - do not invent values */
                temp_stats.gpu_vram_total = -1.0f;
            }
        }
        else if (strncmp(token, "RAM:", 4) == 0) {
            if (sscanf(token + 4, "%f/%f", &temp_stats.ram_used_gb, &temp_stats.ram_total_gb) == 2) {
                fields_parsed++;
            } else {
                temp_stats.ram_used_gb = -1.0f;     /* N/A - do not invent values */
                temp_stats.ram_total_gb = -1.0f;
            }
        }
        else if (strncmp(token, "NET:", 4) == 0) {
            strncpy(temp_stats.net_type, token + 4, sizeof(temp_stats.net_type) - 1);
            temp_stats.net_type[sizeof(temp_stats.net_type) - 1] = '\0';
            fields_parsed++;
        }
        else if (strncmp(token, "SPEED:", 6) == 0) {
            strncpy(temp_stats.net_speed, token + 6, sizeof(temp_stats.net_speed) - 1);
            temp_stats.net_speed[sizeof(temp_stats.net_speed) - 1] = '\0';
            fields_parsed++;
        }
        else if (strncmp(token, "DOWN:", 5) == 0) {
            temp_stats.net_down_mbps = atof(token + 5);
            fields_parsed++;
        }
        else if (strncmp(token, "UP:", 3) == 0) {
            temp_stats.net_up_mbps = atof(token + 3);
            fields_parsed++;
        }
        else if (strncmp(token, "WTEMP:", 6) == 0) {
            temp_stats.weather_temp = (int16_t)atoi(token + 6);
            saw_weather = true;
        }
        else if (strncmp(token, "WCODE:", 6) == 0) {
            temp_stats.weather_code = (int16_t)atoi(token + 6);
            saw_weather = true;
        }
        else if (strncmp(token, "WHUM:", 5) == 0) {
            temp_stats.weather_hum = (int16_t)atoi(token + 5);
            saw_weather = true;
        }
        else if (strncmp(token, "WCITY:", 6) == 0) {
            const char *city = token + 6;
            if (city[0] && strcmp(city, "-") != 0) {
                snprintf(temp_stats.weather_city, sizeof(temp_stats.weather_city), "%s", city);
            }
            saw_weather = true;
        }
        else if (strncmp(token, "SRISE:", 6) == 0) {
            int mins = atoi(token + 6);
            if (mins >= 0 && mins <= 24 * 60) temp_stats.sun_rise_min = (int16_t)mins;
            saw_sun = true;
        }
        else if (strncmp(token, "SSET:", 5) == 0) {
            int mins = atoi(token + 5);
            if (mins >= 0 && mins <= 24 * 60) temp_stats.sun_set_min = (int16_t)mins;
            saw_sun = true;
        }
        else if (strncmp(token, "CLK:", 4) == 0) {
            int year, mon, day, hour, min, sec;
            if (sscanf(token + 4, "%d-%d-%dT%d:%d:%d",
                       &year, &mon, &day, &hour, &min, &sec) == 6 &&
                mon >= 1 && mon <= 12 && day >= 1 && day <= 31 &&
                hour >= 0 && hour <= 23 && min >= 0 && min <= 59 &&
                sec >= 0 && sec <= 59) {
                temp_stats.clock_year = (int16_t)year;
                temp_stats.clock_mon = (int8_t)mon;
                temp_stats.clock_day = (int8_t)day;
                temp_stats.clock_sec = hour * 3600 + min * 60 + sec;
                temp_stats.clock_sync_ms = (uint32_t)(esp_timer_get_time() / 1000);
                saw_clock = true;
            }
        }

        token = strtok(NULL, ",");
    }

    /* Only commit if we got enough fields (avoid partial/corrupt updates) */
    if (fields_parsed >= 5) {
        /* Thread-safe write with timeout - NEVER use portMAX_DELAY! */
        if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(STATS_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            /* Absorb single-sample sensor glitches: replace freshly-arrived
             * N/A fields with the last valid value for a few packets. */
            HOLD_FIELD(temp_stats.cpu_percent, s_pc_stats.cpu_percent, s_hold.cpu_pct);
            HOLD_FIELD(temp_stats.cpu_temp,    s_pc_stats.cpu_temp,    s_hold.cpu_temp);
            HOLD_FIELD(temp_stats.gpu_percent, s_pc_stats.gpu_percent, s_hold.gpu_pct);
            HOLD_FIELD(temp_stats.gpu_temp,    s_pc_stats.gpu_temp,    s_hold.gpu_temp);
            HOLD_FIELD(temp_stats.net_down_mbps, s_pc_stats.net_down_mbps, s_hold.net_dn);
            HOLD_FIELD(temp_stats.net_up_mbps,   s_pc_stats.net_up_mbps,   s_hold.net_up);

            /* VRAM and RAM are used/total pairs - hold both together */
            if (temp_stats.gpu_vram_used < 0) {
                if (s_hold.vram < HOLD_MAX_PACKETS) {
                    temp_stats.gpu_vram_used  = s_pc_stats.gpu_vram_used;
                    temp_stats.gpu_vram_total = s_pc_stats.gpu_vram_total;
                    s_hold.vram++;
                }
            } else {
                s_hold.vram = 0;
            }
            if (temp_stats.ram_used_gb < 0) {
                if (s_hold.ram < HOLD_MAX_PACKETS) {
                    temp_stats.ram_used_gb  = s_pc_stats.ram_used_gb;
                    temp_stats.ram_total_gb = s_pc_stats.ram_total_gb;
                    s_hold.ram++;
                }
            } else {
                s_hold.ram = 0;
            }

            /* Weather rides along every sample. A packet without it keeps
             * the last reading instead of flashing back to N/A. */
            if (!saw_weather) {
                temp_stats.weather_temp = s_pc_stats.weather_temp;
                temp_stats.weather_code = s_pc_stats.weather_code;
                temp_stats.weather_hum = s_pc_stats.weather_hum;
                memcpy(temp_stats.weather_city, s_pc_stats.weather_city, sizeof(temp_stats.weather_city));
            }
            if (!saw_sun) {
                temp_stats.sun_rise_min = s_pc_stats.sun_rise_min;
                temp_stats.sun_set_min = s_pc_stats.sun_set_min;
            }
            /* Between syncs the dial keeps the last stamp and advances it
             * with esp_timer, which follows the 40 MHz crystal. */
            if (!saw_clock || !clock_accept_sync(temp_stats.clock_sec)) {
                clock_keep_last(&temp_stats);
            }

            s_pc_stats = temp_stats;
            s_last_data_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_transport = s_rx_is_ble ? 2 : 1;
            xSemaphoreGive(s_stats_mutex);
            usb_serial_sendf("DATA_OK:%d\n", (int)temp_stats.cpu_percent);
            ESP_LOGD(TAG, "Parsed %d fields, timestamp updated", fields_parsed);
        } else {
            /* Fail-safe: Skip this update, don't freeze! */
            s_stats_mutex_timeouts++;
            ESP_LOGW(TAG, "Stats mutex timeout! Skipping data update. [timeouts: %lu]",
                     (unsigned long)s_stats_mutex_timeouts);
        }
    } else {
        ESP_LOGW(TAG, "Incomplete data: only %d fields parsed, discarding", fields_parsed);
    }
}

/* =============================================================================
 * HANDSHAKE HANDLER (built-in)
 * ========================================================================== */

static bool handle_handshake(const char *line);

int usb_serial_transport(void)
{
    return s_transport;
}

void usb_serial_handle_line(const char *line)
{
    usb_serial_handle_line_from(line, false);
}

void usb_serial_handle_line_from(const char *line, bool from_ble)
{
    if (!line || !line[0]) return;
    s_rx_is_ble = from_ble;
    if (handle_handshake(line)) return;
    for (int h = 0; h < s_handler_count; h++) {
        if (s_handlers[h](line)) return;
    }
    parse_pc_data(line);
}

static bool handle_handshake(const char *line)
{
    if (strcmp(line, "WHO_ARE_YOU?") == 0) {
        hw_identity_t *id = hw_identity_get();
        const esp_app_desc_t *app = esp_app_get_description();
        char response[128];
        /* |V:<version> and |N:<device name> appended in v2.4 -
         * client tolerates their absence (old FW). |N: may be empty. */
        snprintf(response, sizeof(response), "SCARAB_CLIENT_OK|H:%s|V:%s|N:%s\n",
                 id->identity_hash, app->version, id->device_name);
        usb_serial_send(response);
        ESP_LOGI(TAG, "Handshake: WHO_ARE_YOU? -> %s", response);
        return true;
    }
    return false;
}

/* =============================================================================
 * RX TASK
 * ========================================================================== */

static void usb_rx_task(void *arg)
{
    static uint8_t rx_buf[256];
    static char line_buf[USB_LINE_BUFFER_SIZE];
    static int line_pos = 0;
    static bool is_discarding = false;

    ESP_LOGI(TAG, "USB RX Task started");

    /* Subscribe to Task Watchdog - will reset ESP32 if task hangs */
    esp_task_wdt_add(NULL);

    while (1) {
        /* Feed the watchdog at start of each iteration */
        esp_task_wdt_reset();
        /* Read with timeout so other tasks can run */
        int len = usb_serial_jtag_read_bytes(rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t c = rx_buf[i];

                /* End of line - process it or end discard mode */
                if (c == '\n' || c == '\r') {
                    if (is_discarding) {
                        is_discarding = false;
                        line_pos = 0;
                    } else if (line_pos > 0) {
                        line_buf[line_pos] = '\0';

                        /* Try built-in handshake first */
                        usb_serial_handle_line(line_buf);

                        line_pos = 0;
                    }
                }
                /* In discard mode - ignore until newline */
                else if (is_discarding) {
                    /* Do nothing */
                }
                /* Normal character - add to buffer */
                else {
                    if (line_pos < USB_LINE_BUFFER_SIZE - 1) {
                        line_buf[line_pos++] = (char)c;
                    } else {
                        ESP_LOGW(TAG, "Line buffer overflow, discarding rest of line");
                        is_discarding = true;
                    }
                }
            }

            /* Yield to other tasks after processing data (Watchdog friendly) */
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            /* No data - longer delay to save CPU, still yields for Watchdog */
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* Extra yield point for Task Watchdog Timer (TWDT) */
        taskYIELD();
    }
}

/* Task configuration - matches main_lvgl.c defines */
#define STACK_SIZE_USB_RX   8192
#define PRIO_USB_RX         4       /* Highest priority */

void usb_serial_start_rx_task(SemaphoreHandle_t stats_mutex)
{
    s_stats_mutex = stats_mutex;

    /* Create USB RX task with hardened configuration */
    xTaskCreate(usb_rx_task, "usb_rx", STACK_SIZE_USB_RX, NULL, PRIO_USB_RX, NULL);
    ESP_LOGI(TAG, "USB RX Task created (stack: %d, prio: %d)", STACK_SIZE_USB_RX, PRIO_USB_RX);
}
