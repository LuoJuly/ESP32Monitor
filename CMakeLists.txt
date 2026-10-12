# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Custom partition table (OTA app slots + LittleFS storage)
set(PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv")

# Firmware version reported via handshake and GET_FW_VER (embedded in esp_app_desc)
set(PROJECT_VER "2.5.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pc-monitor-poc)
