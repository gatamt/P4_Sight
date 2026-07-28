/**
 * @file ai_task.h
 * @brief Always-on AI inference task — runs 6 models sequentially on P4.
 *
 * The AI task runs independently of video streaming. It grabs the latest
 * camera frame, converts YUV420 to RGB888, runs inference, and writes
 * results to a shared buffer. When an iPhone is connected, bounding boxes
 * are sent as UDP metadata.
 */

#pragma once

#include "ai_detect_types.h"
#include "udp_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the AI inference task.
 *
 * Spawns a FreeRTOS task on core 1 that continuously runs object detection.
 * Must be called AFTER camera capture is running (g_latest_yuv_frame is set).
 *
 * @param streamer  Video streamer handle (for bbox sending to connected
 * client).
 * @return ESP_OK on success.
 */
esp_err_t ai_task_start(udp_streamer_handle_t streamer);

/**
 * @brief Get read-only access to the latest detection results.
 *
 * Uses seqlock pattern — caller should check sequence before/after read.
 */
const ai_result_buffer_t *ai_task_get_results(void);

#ifdef __cplusplus
}
#endif
