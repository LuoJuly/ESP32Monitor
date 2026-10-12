/**
 * Battery level from the onboard AXP2101. The bus is the one the touch
 * controller already uses. Charge current is left at the chip default.
 */

#include "power_axp.h"

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "AXP";

#define AXP_ADDR            0x34
#define AXP_CHIP_ID         0x4A
#define REG_STATUS1         0x00
#define REG_STATUS2         0x01
#define REG_IC_TYPE         0x03
#define REG_ADC_EN          0x30
#define REG_VBAT_H          0x34
#define REG_VBAT_L          0x35
#define REG_BAT_DET         0x68
#define REG_PERCENT         0xA4

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static bool s_missing;

static esp_err_t reg_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 50);
}

static esp_err_t reg_set_bit(uint8_t reg, uint8_t bit)
{
    uint8_t value = 0;
    esp_err_t err = reg_read(reg, &value);
    if (err != ESP_OK) return err;
    value |= (uint8_t)(1u << bit);
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static int percent_from_mv(int mv)
{
    if (mv >= 4150) return 100;
    if (mv <= 3400) return 0;
    return (mv - 3400) * 100 / 750;
}

static bool ensure_device(void)
{
    if (s_ready) return true;
    if (s_missing) return false;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c %d", err);
        s_missing = true;
        return false;
    }

    uint8_t id = 0;
    if (reg_read(REG_IC_TYPE, &id) != ESP_OK || id != AXP_CHIP_ID) {
        ESP_LOGW(TAG, "chip %02x", id);
        s_missing = true;
        return false;
    }
    reg_set_bit(REG_BAT_DET, 0);
    reg_set_bit(REG_ADC_EN, 0);
    s_ready = true;
    return true;
}

bool power_axp_read(power_batt_t *out)
{
    if (!out || !ensure_device()) return false;

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t percent = 0;
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (reg_read(REG_STATUS1, &status1) != ESP_OK ||
        reg_read(REG_STATUS2, &status2) != ESP_OK ||
        reg_read(REG_PERCENT, &percent) != ESP_OK ||
        reg_read(REG_VBAT_H, &hi) != ESP_OK ||
        reg_read(REG_VBAT_L, &lo) != ESP_OK) {
        return false;
    }

    power_batt_t sample = {0};
    sample.present = (status1 & (1u << 3)) != 0;
    sample.charging = (status2 >> 5) == 1;
    if (!sample.present) {
        sample.percent = -1;
        *out = sample;
        return true;
    }

    int mv = ((hi & 0x1F) << 8) | lo;
    int gauge = percent;
    if (gauge > 100) gauge = 100;
    /* A gauge that is still at 0 while the cell is well above empty has not
     * settled. The cell voltage is the reading until it does. */
    if (gauge == 0 && mv >= 3600) gauge = percent_from_mv(mv);
    sample.percent = gauge;
    *out = sample;
    return true;
}
