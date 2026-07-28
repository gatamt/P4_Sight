/**
 * @file yuv_to_rgb_hw.h
 * @brief O_UYY_E_VYY → RGB888 conversion with integrated decimation.
 *
 * The ESP32-P4 ISP outputs YUV420 in the proprietary O_UYY_E_VYY packed
 * format — NOT standard I420/NV12/NV21.  Layout:
 *
 *   Even rows: V Y Y V Y Y V Y Y ...   (3 bytes per 2 pixels)
 *   Odd  rows: U Y Y U Y Y U Y Y ...
 *   Stride = width × 3/2              (1920 bytes for 1280 wide)
 *
 * This module decodes the format correctly with BT.601 limited-range
 * coefficients and combines conversion + decimation in a single pass.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the converter (currently a no-op, kept for API compat).
 * @return ESP_OK always.
 */
esp_err_t yuv_to_rgb_hw_init(void);

/**
 * @brief Allocate an RGB888 output buffer in PSRAM.
 *
 * Caller frees with heap_caps_free().
 *
 * @param width    Output width in pixels.
 * @param height   Output height in pixels.
 * @param[out] out_buf   Returned pointer.
 * @param[out] out_size  Returned size in bytes.
 * @return ESP_OK or ESP_ERR_NO_MEM.
 */
esp_err_t yuv_to_rgb_hw_alloc_buf(uint32_t width, uint32_t height,
                                  uint8_t **out_buf, uint32_t *out_size);

/**
 * @brief Convert O_UYY_E_VYY frame to RGB888 at an arbitrary target size.
 *
 * Single-pass: reads source pixels at decimated positions, decodes
 * Y/U/V from the packed layout, applies BT.601, writes RGB888.
 *
 * Caller MUST have called esp_cache_msync(M2C) on @p yuv_data first.
 *
 * @param yuv_data  Source O_UYY_E_VYY frame (ISP output, PSRAM).
 * @param src_w     Source width  (e.g. 1280).
 * @param src_h     Source height (e.g. 720).
 * @param rgb_out   Destination RGB888 buffer (from yuv_to_rgb_hw_alloc_buf).
 * @param dst_w     Target width  (e.g. 224 or 320).
 * @param dst_h     Target height (e.g. 224 or 320).
 * @return ESP_OK on success.
 */
esp_err_t yuv_to_rgb_hw_convert(const uint8_t *yuv_data, uint32_t src_w,
                                uint32_t src_h, uint8_t *rgb_out,
                                uint32_t dst_w, uint32_t dst_h);

/**
 * @brief Release resources (currently a no-op).
 */
void yuv_to_rgb_hw_deinit(void);

#ifdef __cplusplus
}
#endif
