idf_component_register(
    SRCS
        # Main application
        "main_lvgl.c"

        # Drivers
        "drivers/usb_serial_comm.c"
        "drivers/ble_link.c"
        "drivers/fw_update.c"
        "drivers/display_board.c"
        "drivers/power_axp.c"

        # Storage modules
        "storage/storage_mgr.c"
        "storage/hw_identity.c"
        "gui_settings.c"

        # UI modules
        "ui/ui_manager.c"
        "ui/screensaver_mgr.c"

        # Screen implementations
        "screens/screen_weather_lvgl.c"
        "screens/screen_clock_lvgl.c"
        "screens/screen_cpu_lvgl.c"
        "screens/screen_gpu_lvgl.c"
        "screens/screen_ram_lvgl.c"
        "screens/screen_network_lvgl.c"

        # Compiled images (system icons - Desert-Spec v2.2)
        "images/CPU.c"
        "images/GPU.c"
        "images/RAM.c"
        "images/NET.c"

    INCLUDE_DIRS
        "."
        "core"
        "screens"
        "storage"
        "drivers"
        "ui"

    REQUIRES
        lvgl
        esp_timer
        driver
        bt
        nvs_flash
        app_update
        esp_app_format
        esp_lcd
        espressif__esp_lcd_co5300
        espressif__esp_lvgl_adapter
        waveshare__esp32_s3_touch_amoled_1_75c
)

# Build the LittleFS image but do NOT flash it with 'idf.py flash'.
# User data (uploaded screensaver images, theme colors, hardware names) must
# survive firmware updates. LittleFS auto-formats an empty partition on first
# boot (format_if_mount_failed=true), so flashing storage.bin is optional.
# To provision it manually once:
#   esptool.py write_flash 0xC20000 build/storage.bin   (offset: see partitions.csv)
littlefs_create_partition_image(storage lfs)
