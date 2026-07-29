/**
 * @file mdns_announce.h
 * @brief Bonjour/mDNS service announcement for _gatavideo._udp.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start mDNS and announce the _gatavideo._udp service.
 *
 * TXT records include port, codec, resolution, fps, and protocol version
 * so the iPhone app can discover the board without a hardcoded IP.
 *
 * @return ESP_OK on success, error otherwise.
 */
esp_err_t mdns_video_announce(void);

#ifdef __cplusplus
}
#endif
