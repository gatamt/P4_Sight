/**
 * @file wifi_station.h
 * @brief Wi-Fi AP mode — C6 creates network, iPhone connects to it.
 */

#pragma once

#include "esp_err.h"
#include "udp_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi in AP mode.
 *
 * C6 co-processor creates a SoftAP network. iPhone connects to it.
 * Blocks until AP is started and ready for connections.
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_init(void);

/**
 * @brief Register the UDP streamer to receive station disconnect events.
 *
 * Call after both wifi_ap_init() and udp_stream_start() have succeeded.
 */
void wifi_ap_set_streamer(udp_streamer_handle_t streamer);

#ifdef __cplusplus
}
#endif
