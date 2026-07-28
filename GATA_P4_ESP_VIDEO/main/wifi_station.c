/**
 * @file wifi_station.c
 * @brief Wi-Fi AP mode for ESP32-P4 via esp_wifi_remote (C6 co-processor).
 *
 * AP-mode: C6 creates its own network, iPhone connects to it.
 * This avoids DHCP-client forwarding issues over the ESP-Hosted SDIO bridge.
 * The board provides DHCP locally and the iPhone joins the resulting network.
 */

#include "wifi_station.h"
#include "udp_stream.h"
#include "wifi_cfg.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "wifi-ap";

#define WIFI_AP_READY_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static int s_station_count = 0;
static udp_streamer_handle_t s_streamer = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_AP_START:
      ESP_LOGI(TAG, "AP started");
      xEventGroupSetBits(s_wifi_event_group, WIFI_AP_READY_BIT);
      break;
    case WIFI_EVENT_AP_STOP:
      ESP_LOGW(TAG, "AP stopped");
      xEventGroupClearBits(s_wifi_event_group, WIFI_AP_READY_BIT);
      break;
    case WIFI_EVENT_AP_STACONNECTED: {
      wifi_event_ap_staconnected_t *evt =
          (wifi_event_ap_staconnected_t *)event_data;
      s_station_count++;
      ESP_LOGI(TAG, "Station connected (aid=%d, total=%d)", evt->aid,
               s_station_count);
      break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
      wifi_event_ap_stadisconnected_t *evt =
          (wifi_event_ap_stadisconnected_t *)event_data;
      if (s_station_count > 0) {
        s_station_count--;
      }
      ESP_LOGI(TAG, "Station disconnected (aid=%d, total=%d)", evt->aid,
               s_station_count);
      if (s_station_count == 0 && s_streamer != NULL) {
        udp_stream_on_station_disconnect(s_streamer);
      }
      break;
    }
    default:
      break;
    }
  }
}

esp_err_t wifi_ap_init(void) {
  ESP_LOGI(TAG, "Initializing Wi-Fi AP mode");

  /* NVS required by Wi-Fi driver */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
    ret = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(ret, TAG, "NVS init failed");

  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                      "event loop failed");

  /* Create AP netif with custom static IP */
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  if (ap_netif == NULL) {
    ESP_LOGE(TAG, "Failed to create AP netif");
    return ESP_FAIL;
  }

  /* Set static IP for the AP */
  esp_netif_ip_info_t ip_info = {0};
  ip4addr_aton(WIFI_AP_IP, (ip4_addr_t *)&ip_info.ip);
  ip_info.gw = ip_info.ip;
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

  ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), TAG, "DHCPS stop failed");
  ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), TAG,
                      "set IP failed");
  ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG,
                      "DHCPS start failed");

  /* Wi-Fi init */
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = 0;
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

  /* Register event handlers */
  esp_event_handler_instance_t inst;
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          &wifi_event_handler, NULL, &inst),
      TAG, "event register failed");

  /* AP configuration */
  wifi_config_t ap_config = {0};
  strncpy((char *)ap_config.ap.ssid, WIFI_AP_SSID,
          sizeof(ap_config.ap.ssid) - 1);
  strncpy((char *)ap_config.ap.password, WIFI_AP_PASSWORD,
          sizeof(ap_config.ap.password) - 1);
  ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
  ap_config.ap.channel = WIFI_AP_CHANNEL;
  ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  if (strlen(WIFI_AP_PASSWORD) < 8) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                      "set storage failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                      "set config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

  /* Performance: max TX power, no power save */
  esp_wifi_set_max_tx_power(80); /* 20 dBm max */
  esp_wifi_set_ps(WIFI_PS_NONE);

  int8_t actual_power = 0;
  esp_wifi_get_max_tx_power(&actual_power);
  ESP_LOGI(TAG, "TX power: %d (0.25dBm units) = %.1f dBm", (int)actual_power,
           actual_power * 0.25f);

  /* Wait for AP to start */
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_AP_READY_BIT, pdFALSE,
                          pdFALSE, pdMS_TO_TICKS(10000));

  if (!(bits & WIFI_AP_READY_BIT)) {
    ESP_LOGE(TAG, "AP start timeout");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AP ready: SSID='%s' PWD='%s' CH=%d IP=%s", WIFI_AP_SSID,
           WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, WIFI_AP_IP);
  return ESP_OK;
}

void wifi_ap_set_streamer(udp_streamer_handle_t streamer) {
  s_streamer = streamer;
}
