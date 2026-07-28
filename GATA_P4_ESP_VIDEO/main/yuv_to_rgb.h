/**
 * @file yuv_to_rgb.h
 * @brief Legacy NV12-to-RGB888 conversion with integrated decimation.
 *
 * This interface is retained for the conventional NV12 converter in
 * yuv_to_rgb.c. The verified IMX708 path uses yuv_to_rgb_hw.h for the packed
 * O_UYY_E_VYY ISP output. Callers reading a DMA-backed source buffer must
 * synchronize the cache before conversion.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a conventional NV12 frame to RGB888 with decimation.
 *
 * Uses nearest-neighbor sampling and limited-range BT.601 coefficients. No
 * intermediate full-resolution buffer is allocated; output is written
 * directly at the destination resolution.
 *
 * @param yuv_data  Source NV12 data (Y plane followed by interleaved UV).
 * @param src_w     Source width (e.g. 1280).
 * @param src_h     Source height (e.g. 720).
 * @param rgb_out   Output RGB888 buffer (must be dst_w * dst_h * 3 bytes).
 * @param dst_w     Destination width (e.g. 224).
 * @param dst_h     Destination height (e.g. 224).
 */
void yuv420_to_rgb888_decimate(const uint8_t *yuv_data, uint32_t src_w,
                               uint32_t src_h, uint8_t *rgb_out, uint32_t dst_w,
                               uint32_t dst_h);

#ifdef __cplusplus
}
#endif
