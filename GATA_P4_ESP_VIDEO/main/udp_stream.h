/**
 * @file udp_stream.h
 * @brief Chunked UDP H.264 streaming with heartbeat-based client lifecycle.
 *
 * Protocol messages (4-byte ASCII, app -> firmware):
 *   VID0 — Register for video (start streaming)
 *   BEAT — Heartbeat keepalive (every 1s from app)
 *   PAWS — Pause (app going to background)
 *   GONE — Disconnect (app shutting down)
 *
 * Firmware -> app:
 *   VACK — VID0 acknowledged, streaming will begin
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declaration — full definition in <lwip/sockets.h>.
 * DO NOT include lwip/sockets.h here: it redefines _IOR/_IOW macros
 * which breaks V4L2 ioctl command numbers in camera_bringup.c. */
struct sockaddr_in;

#ifdef __cplusplus
extern "C" {
#endif

/** Streaming state machine states. */
typedef enum {
  STREAM_IDLE,   /**< No client — camera DMA runs, encode+send skipped */
  STREAM_ACTIVE, /**< Client registered, heartbeats arriving — full pipeline */
  STREAM_PAUSED, /**< Client sent PAWS — DMA runs, encode+send skipped */
} udp_stream_state_t;

/**
 * @brief 28-byte H.264 chunk header — little-endian, matches iPhone app parser.
 *
 * Each UDP datagram carries this header + up to ~1400 bytes of H.264 payload.
 */
typedef struct __attribute__((packed)) {
  uint32_t frame_id;
  uint16_t width;
  uint16_t height;
  uint32_t timestamp;
  uint32_t total_len;
  uint16_t chunk_idx;
  uint16_t chunk_count;
  uint32_t frame_type; /**< 1 = keyframe (IDR), 0 = P-frame */
  uint32_t reserved;
} h264_chunk_header_t;

_Static_assert(sizeof(h264_chunk_header_t) == 28,
               "H264 header must be 28 bytes");

/** Opaque streamer handle. */
typedef struct udp_streamer *udp_streamer_handle_t;

/**
 * @brief Start the UDP video streamer.
 *
 * Binds a UDP socket on VIDEO_UDP_PORT and spawns a listener task
 * that handles VID0/BEAT/PAWS/GONE control messages.
 *
 * @param out Returned streamer handle.
 * @return ESP_OK on success.
 */
esp_err_t udp_stream_start(udp_streamer_handle_t *out);

/**
 * @brief Send one encoded H.264 frame as chunked UDP datagrams.
 *
 * Only sends if state is STREAM_ACTIVE.
 *
 * @param streamer   Streamer handle.
 * @param h264_data  Pointer to encoded H.264 data.
 * @param h264_size  Size of the H.264 data in bytes.
 * @param frame_id   Monotonically increasing frame counter.
 * @param width      Frame width.
 * @param height     Frame height.
 * @param is_keyframe True if this is an IDR/keyframe.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no active client.
 */
esp_err_t udp_stream_send_frame(udp_streamer_handle_t streamer,
                                const uint8_t *h264_data, uint32_t h264_size,
                                uint32_t frame_id, uint16_t width,
                                uint16_t height, bool is_keyframe);

/**
 * @brief Get current streaming state.
 */
udp_stream_state_t udp_stream_get_state(udp_streamer_handle_t streamer);

/**
 * @brief Check and clear the force-IDR flag (set after resume from PAUSED).
 *
 * @return true if an IDR should be forced on the next encode.
 */
bool udp_stream_should_force_idr(udp_streamer_handle_t streamer);

/**
 * @brief Notify the streamer that a Wi-Fi station disconnected.
 *
 * Transitions to STREAM_IDLE immediately.
 */
void udp_stream_on_station_disconnect(udp_streamer_handle_t streamer);

/**
 * @brief Get the currently registered client's address.
 *
 * Used by the bbox sender to know where to send detection data.
 *
 * @param streamer  Streamer handle.
 * @param out_addr  Output: filled with client's sockaddr_in.
 * @return true if a client is registered (state != IDLE), false otherwise.
 */
bool udp_stream_get_client_addr(udp_streamer_handle_t streamer,
                                struct sockaddr_in *out_addr);

/**
 * @brief Stop the streamer and free resources.
 */
void udp_stream_stop(udp_streamer_handle_t streamer);

#ifdef __cplusplus
}
#endif
