/**
 * @file bbox_sender.h
 * @brief Send AI detection results as UDP bounding box packets.
 */

#pragma once

#include "ai_detect_types.h"
#include "esp_err.h"

/* Forward declaration — full definition in <lwip/sockets.h>.
 * DO NOT include lwip/sockets.h in headers: it redefines _IOR/_IOW. */
struct sockaddr_in;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the bbox sender (creates UDP socket).
 *
 * @return ESP_OK on success.
 */
esp_err_t bbox_sender_init(void);

/**
 * @brief Send detection results to the given client address on BBOX_UDP_PORT.
 *
 * Serializes the result buffer into a single UDP packet and sends it.
 * Safe to call frequently — each call is a single sendto().
 *
 * @param results     Detection results to send.
 * @param client_addr Client's IP address (port is overridden to BBOX_UDP_PORT).
 * @return ESP_OK on success.
 */
esp_err_t bbox_sender_send(const ai_result_buffer_t *results,
                           const struct sockaddr_in *client_addr);

#ifdef __cplusplus
}
#endif
