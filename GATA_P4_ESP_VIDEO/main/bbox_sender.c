/**
 * @file bbox_sender.c
 * @brief Serialize and send AI detection results over UDP.
 */

#include "bbox_sender.h"
#include "wifi_cfg.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "bbox-send";

static int s_bbox_sock = -1;

esp_err_t bbox_sender_init(void) {
  s_bbox_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_bbox_sock < 0) {
    ESP_LOGE(TAG, "Failed to create bbox socket: %s", strerror(errno));
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Bbox sender initialized (sock=%d)", s_bbox_sock);
  return ESP_OK;
}

esp_err_t bbox_sender_send(const ai_result_buffer_t *results,
                           const struct sockaddr_in *client_addr) {
  if (s_bbox_sock < 0 || results == NULL || client_addr == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t num = results->num_detections;
  if (num > AI_MAX_DETECTIONS) {
    num = AI_MAX_DETECTIONS;
  }

  /* Build packet: header + detections */
  uint8_t pkt[sizeof(bbox_packet_header_t) +
              AI_MAX_DETECTIONS * sizeof(bbox_detection_wire_t)];
  bbox_packet_header_t *hdr = (bbox_packet_header_t *)pkt;

  memcpy(hdr->magic, "BBOX", 4);
  hdr->frame_id = results->frame_id;
  hdr->timestamp_ms = results->timestamp_ms;
  hdr->num_detections = num;

  bbox_detection_wire_t *wire =
      (bbox_detection_wire_t *)(pkt + sizeof(bbox_packet_header_t));

  for (uint8_t i = 0; i < num; i++) {
    const ai_detection_t *det = &results->detections[i];
    wire[i].class_id = det->class_id;
    wire[i].confidence = det->confidence;
    wire[i].x1 = det->x1;
    wire[i].y1 = det->y1;
    wire[i].x2 = det->x2;
    wire[i].y2 = det->y2;

    size_t label_len = strlen(det->label);
    if (label_len > 15) {
      label_len = 15;
    }
    wire[i].label_len = (uint8_t)label_len;
    memset(wire[i].label, 0, 15);
    memcpy(wire[i].label, det->label, label_len);
  }

  uint32_t pkt_len =
      sizeof(bbox_packet_header_t) + num * sizeof(bbox_detection_wire_t);

  /* Override port to BBOX_UDP_PORT */
  struct sockaddr_in dest = *client_addr;
  dest.sin_port = htons(BBOX_UDP_PORT);

  int sent = sendto(s_bbox_sock, pkt, pkt_len, 0, (struct sockaddr *)&dest,
                    sizeof(dest));
  if (sent < 0) {
    ESP_LOGW(TAG, "bbox sendto failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  return ESP_OK;
}
