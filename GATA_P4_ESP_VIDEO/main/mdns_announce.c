/**
 * @file mdns_announce.c
 * @brief mDNS service announcement for video discovery.
 *
 * Announces _gatavideo._udp so the iPhone app can find the board
 * on the hotspot network without a hardcoded IP address.
 */

#include "mdns_announce.h"
#include "wifi_cfg.h"

#include "esp_check.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns-video";

esp_err_t mdns_video_announce(void) {
  ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init failed");

  ESP_RETURN_ON_ERROR(mdns_hostname_set("gata-p4"), TAG,
                      "mDNS hostname failed");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set("GATA P4 Video Streamer"), TAG,
                      "mDNS instance name failed");

  mdns_txt_item_t txt_items[] = {
      {"port", "3334"},  {"codec", "h264"}, {"width", "1280"},
      {"height", "720"}, {"fps", "30"},     {"proto", "1"},
  };

  ESP_RETURN_ON_ERROR(
      mdns_service_add("GATA Video", "_gatavideo", "_udp", VIDEO_UDP_PORT,
                       txt_items, sizeof(txt_items) / sizeof(txt_items[0])),
      TAG, "mDNS service add failed");

  ESP_LOGI(TAG, "mDNS: announced _gatavideo._udp on port %d", VIDEO_UDP_PORT);
  return ESP_OK;
}
