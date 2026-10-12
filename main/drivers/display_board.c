/**
 * CO5300 on the Waveshare 1.75C.
 * Draw buffers stay in PSRAM. Each SPI chunk stays within 8 KB.
 * Direct PSRAM DMA of a large transfer underruns, and IDF treats that as a
 * failed transfer, so the next command never goes out and the panel stays black.
 *
 * The picture is rotated here, not by the panel. MADCTL would turn the
 * factory 6 px column margin onto the bottom of the round glass, and those
 * unwritten pixels stay bright green.
 */

#include "display_board.h"

#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "bsp/touch.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

#include <string.h>

static const char *TAG = "display";

/* Full-screen strips. A 96-row strip is a vertical slice after the 90 degree
 * rotate, so a sideways swipe looked cut into pieces. One flush covers the
 * whole dirty area instead. */
#define DRAW_BUFFER_HEIGHT          BSP_LCD_V_RES
#define LCD_PCLK_HZ                 (40 * 1000 * 1000)
#define ROT_TILE                    32
/* Copied into internal RAM before DMA. Must stay small enough to allocate. */
#define LCD_DMA_CHUNK_BYTES         (8 * 1024)
/* One LVGL flush is at most the full logical frame. */
#define ROT_CAP_PX ((size_t)BSP_LCD_H_RES * DRAW_BUFFER_HEIGHT)

static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_rot_buf;
/* Source tile, internal RAM, so the rotate reads PSRAM in cache-line bursts. */
static uint16_t s_tile[ROT_TILE * ROT_TILE];
static bool s_revealed;
static uint8_t s_brightness = 100;

/* Logical pixel (lx, ly) lands at panel (W-1-ly, lx): 90 degrees clockwise.
 * Tiles are copied in row order, then written as dest-row runs. */
static esp_err_t draw_rotated_cw(lv_display_t *disp, esp_lcd_panel_handle_t panel,
                                 int x_start, int y_start, int x_end, int y_end,
                                 const void *color_map, void *user_ctx)
{
    (void)disp;
    (void)user_ctx;
    const int src_w = x_end - x_start;
    const int src_h = y_end - y_start;
    if (src_w <= 0 || src_h <= 0 || !s_rot_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)src_w * (size_t)src_h > ROT_CAP_PX) {
        ESP_LOGE(TAG, "flush %dx%d exceeds rotate buffer", src_w, src_h);
        return ESP_ERR_NO_MEM;
    }

    const uint16_t *src = color_map;
    const int px0 = BSP_LCD_H_RES - y_end;
    const int py0 = x_start;
    const int dst_w = src_h;
    const int dst_h = src_w;

    for (int row0 = 0; row0 < dst_h; row0 += ROT_TILE) {
        int tw = dst_h - row0;
        if (tw > ROT_TILE) tw = ROT_TILE;
        for (int col0 = 0; col0 < dst_w; col0 += ROT_TILE) {
            int th = dst_w - col0;
            if (th > ROT_TILE) th = ROT_TILE;
            const int ly0 = y_end - (col0 + th);
            for (int r = 0; r < th; r++) {
                const uint16_t *srow = src + (size_t)(ly0 + r - y_start) * src_w + row0;
                memcpy(s_tile + r * ROT_TILE, srow, (size_t)tw * sizeof(uint16_t));
            }
            for (int c = 0; c < tw; c++) {
                uint16_t *drow = s_rot_buf + (size_t)(row0 + c) * dst_w + col0;
                for (int r = 0; r < th; r++) {
                    drow[r] = s_tile[(th - 1 - r) * ROT_TILE + c];
                }
            }
        }
    }

    return esp_lcd_panel_draw_bitmap(panel, px0, py0, px0 + dst_w, py0 + dst_h, s_rot_buf);
}

static const uint8_t k_page20[] = {0x20};
static const uint8_t k_gate_voltage[] = {0x10};
static const uint8_t k_source_voltage[] = {0xA0};
static const uint8_t k_page00[] = {0x00};
static const uint8_t k_gamma[] = {0x80};
static const uint8_t k_rgb565[] = {0x55};
static const uint8_t k_tearing[] = {0x00};
static const uint8_t k_write_ctrl[] = {0x20};
static const uint8_t k_brightness_off[] = {0x00};
static const uint8_t k_reg63[] = {0xFF};
static const uint8_t k_column[] = {0x00, 0x06, 0x01, 0xD7};
static const uint8_t k_row[] = {0x00, 0x00, 0x01, 0xD1};

static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, k_page20, sizeof(k_page20), 0},
    {0x19, k_gate_voltage, sizeof(k_gate_voltage), 0},
    {0x1C, k_source_voltage, sizeof(k_source_voltage), 0},
    {0xFE, k_page00, sizeof(k_page00), 0},
    {0xC4, k_gamma, sizeof(k_gamma), 0},
    {0x3A, k_rgb565, sizeof(k_rgb565), 0},
    {0x35, k_tearing, sizeof(k_tearing), 0},
    {0x53, k_write_ctrl, sizeof(k_write_ctrl), 0},
    {0x51, k_brightness_off, sizeof(k_brightness_off), 0},
    {0x63, k_reg63, sizeof(k_reg63), 0},
    {0x2A, k_column, sizeof(k_column), 0},
    {0x2B, k_row, sizeof(k_row), 600},
    {0x11, NULL, 0, 600},
};

static esp_err_t init_hidden_panel(size_t max_transfer, esp_lcd_panel_io_handle_t *io_out)
{
    spi_bus_config_t bus = {
        .data0_io_num = BSP_LCD_DATA0,
        .data1_io_num = BSP_LCD_DATA1,
        .sclk_io_num = BSP_LCD_PCLK,
        .data2_io_num = BSP_LCD_DATA2,
        .data3_io_num = BSP_LCD_DATA3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = max_transfer,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO),
                        TAG, "QSPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = 1,
            .psram_dma_direct = 0,
        },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM,
                                                &io_cfg, &io),
                        TAG, "panel IO init failed");

    co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_cfg, &s_panel),
                        TAG, "CO5300 create failed");
    /* Factory window: column 6 is the first visible pixel. Leave MADCTL
     * alone so that margin stays off the round glass. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0x06, 0), TAG, "gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "panel stayed on");
    *io_out = io;
    return ESP_OK;
}

static void round_draw_area(lv_area_t *area, void *user_data)
{
    (void)user_data;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

lv_display_t *display_board_init(void)
{
    if (s_display) {
        return s_display;
    }

    const esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&adapter) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed");
        return NULL;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    if (init_hidden_panel(LCD_DMA_CHUNK_BYTES, &io) != ESP_OK) {
        return NULL;
    }

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            s_panel, io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    disp_cfg.profile.buffer_height = DRAW_BUFFER_HEIGHT;
    disp_cfg.profile.use_psram = true;
    disp_cfg.profile.require_double_buffer = true;
    disp_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;

    s_display = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_display) {
        ESP_LOGE(TAG, "display register failed");
        return NULL;
    }
    if (esp_lv_adapter_set_area_rounder_cb(s_display, round_draw_area, NULL) != ESP_OK) {
        s_display = NULL;
        return NULL;
    }
    s_rot_buf = heap_caps_aligned_alloc(64, ROT_CAP_PX * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rot_buf) {
        ESP_LOGE(TAG, "rotate buffer alloc failed");
        s_display = NULL;
        return NULL;
    }
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = {
        .custom_draw_bitmap = draw_rotated_cw,
    };
    if (esp_lv_adapter_set_draw_bitmap_callbacks(s_display, &draw_cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "rotate callback failed");
        s_display = NULL;
        return NULL;
    }

    bsp_display_cfg_t touch_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    esp_lcd_touch_handle_t touch = NULL;
    if (bsp_touch_new(&touch_cfg, &touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed");
        s_display = NULL;
        return NULL;
    }
    const esp_lv_adapter_touch_config_t touch_reg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, touch);
    if (!esp_lv_adapter_register_touch(&touch_reg)) {
        ESP_LOGE(TAG, "touch register failed");
        s_display = NULL;
        return NULL;
    }

    lv_obj_t *startup = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(startup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(startup, LV_OPA_COVER, 0);
    lv_obj_remove_flag(startup, LV_OBJ_FLAG_SCROLLABLE);

    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL task start failed");
        s_display = NULL;
        return NULL;
    }

    ESP_LOGI(TAG, "CO5300 ready %dx%d, %u-row PSRAM buffers",
             BSP_LCD_H_RES, BSP_LCD_V_RES, DRAW_BUFFER_HEIGHT);
    return s_display;
}

esp_err_t display_board_reveal(void)
{
    if (!s_panel || !s_display) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_revealed) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "reveal failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness), TAG, "brightness failed");
    s_revealed = true;
    return ESP_OK;
}

esp_err_t display_board_set_awake(bool awake)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (awake) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(s_panel, s_brightness), TAG, "brightness failed");
        s_revealed = true;
        return ESP_OK;
    }
    /* DISPOFF, not brightness 0. Brightness 0 keeps the panel scanning and
     * the CO5300 flashes white between frames. */
    return esp_lcd_panel_disp_on_off(s_panel, false);
}

void display_board_set_brightness(uint8_t percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (s_panel && s_revealed) {
        esp_lcd_panel_co5300_set_brightness(s_panel, percent);
    }
}

uint8_t display_board_get_brightness(void)
{
    return s_brightness;
}
