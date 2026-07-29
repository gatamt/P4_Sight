/**
 * @file h264_encoder.h
 * @brief H.264 M2M encoder using /dev/video11 (ESP32-P4 HW encoder).
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque encoder handle. */
typedef struct h264_enc *h264_enc_handle_t;

/**
 * @brief Open the H.264 M2M encoder for the given frame geometry.
 *
 * @param width  Frame width (must match capture, e.g. 1280).
 * @param height Frame height (must match capture, e.g. 720).
 * @param out    Returned encoder handle.
 * @return ESP_OK on success.
 */
esp_err_t h264_enc_open(uint32_t width, uint32_t height,
                        h264_enc_handle_t *out);

/**
 * @brief Encode one YUV420 frame to H.264.
 *
 * @param enc          Encoder handle from h264_enc_open().
 * @param yuv_data     Pointer to YUV420 input buffer.
 * @param yuv_size     Size of the YUV420 buffer in bytes.
 * @param out_data     Pointer to receive H.264 output buffer (owned by encoder,
 * valid until next call).
 * @param out_size     Receives the size of the encoded H.264 data.
 * @param is_keyframe  Set to true if the encoded frame is an IDR/keyframe.
 * @return ESP_OK on success.
 */
esp_err_t h264_enc_process(h264_enc_handle_t enc, const uint8_t *yuv_data,
                           uint32_t yuv_size, uint8_t **out_data,
                           uint32_t *out_size, bool *is_keyframe);

/**
 * @brief Force the next encoded frame to be an IDR keyframe.
 *
 * Used after stream resume so the decoder can start immediately.
 *
 * @param enc Encoder handle.
 */
void h264_enc_force_idr(h264_enc_handle_t enc);

/**
 * @brief Close the H.264 encoder and free resources.
 *
 * @param enc Encoder handle.
 */
void h264_enc_close(h264_enc_handle_t enc);

#ifdef __cplusplus
}
#endif
