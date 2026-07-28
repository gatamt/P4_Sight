/**
 * @file yuv_to_rgb.h
 * @brief YUV420 planar to RGB888 decimating conversion for AI inference input.
 *
 * IMPORTANT: Caller MUST call esp_cache_msync() on the YUV source buffer
 * before calling yuv420_to_rgb888_decimate(). The P4's L2 cache may hold
 * stale data from DMA writes otherwise.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert YUV420 planar to RGB888 with decimation in a single pass.
 *
 * Uses nearest-neighbor sampling and BT.601 coefficients. No intermediate
 * full-resolution buffer is allocated — output is produced directly at
 * the destination resolution.
 *
 * @param yuv_data  Source YUV420 planar data (Y plane, then U, then V).
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
