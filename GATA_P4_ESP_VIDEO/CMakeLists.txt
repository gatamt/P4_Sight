cmake_minimum_required(VERSION 3.16)

# ── IDF_PATH verification gate ──
# Abort if IDF_PATH does not point to the v5.4.1 release tree.
# This prevents accidental builds against v5.5.3 or other versions.
if(NOT "$ENV{IDF_PATH}" MATCHES "v5.4.1")
    message(FATAL_ERROR
        "IDF_PATH verification failed!\n"
        "  Expected: path containing 'v5.4.1'\n"
        "  Actual:   $ENV{IDF_PATH}\n"
        "This project MUST be built against ESP-IDF v5.4.1.\n"
        "Run: export IDF_PATH=~/esp/v5.4.1_release/esp-idf-v5.4.1 && . $IDF_PATH/export.sh")
endif()

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-video-components/esp_cam_sensor"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-video-components/esp_sccb_intf"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-video-components/esp_video"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-video-components/esp_ipa"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(gata_p4_esp_video_imx708)
