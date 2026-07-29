/**
 * @file udp_stream.c
 * @brief Chunked UDP H.264 video streaming with heartbeat-based client
 * lifecycle.
 *
 * Protocol (4-byte ASCII over UDP port 3334):
 *
 * App -> Firmware:
 *   VID0  Start streaming (register client)
 *   BEAT  Heartbeat keepalive (app sends every 1s)
 *   PAWS  Pause (app going to background)
 *   GONE  Disconnect (app shutting down)
 *
 * Firmware -> App:
 *   VACK  VID0 acknowledged
 *
 * State machine:
 *   IDLE ──VID0──> ACTIVE ──PAWS──> PAUSED
 *     ^              |                 |
 *     |   timeout/GONE/STA_DISC    BEAT│
 *     +──────────────+                 v
 *     ^                            ACTIVE
 *     |        timeout/GONE/STA_DISC  |
 *     +───────────── PAUSED ──────────+
 */

#include "udp_stream.h"
#include "wifi_cfg.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "udp-stream";

#define MAX_PAYLOAD_SIZE 1400
#define LISTENER_STACK 4096

/* Control message magic strings (4 bytes each) */
#define MSG_VID0 "VID0"
#define MSG_BEAT "BEAT"
#define MSG_PAWS "PAWS"
#define MSG_GONE "GONE"
#define MSG_VACK "VACK"
#define MSG_LEN 4

/** Heartbeat timeout: 3 seconds without BEAT/VID0 -> IDLE.
 *  Tolerates up to 2 consecutive lost UDP packets at 1s interval. */
#define HEARTBEAT_TIMEOUT_US (3 * 1000 * 1000)

struct udp_streamer {
  int sock;
  struct sockaddr_in client_addr;
  volatile udp_stream_state_t state;
  volatile int64_t last_heartbeat_us;
  volatile bool force_idr;
  TaskHandle_t listener_task;
};

/**
 * @brief Send a 4-byte control response to the registered client.
 */
static void send_control(struct udp_streamer *s, const char *msg) {
  if (s->sock < 0) {
    return;
  }
  sendto(s->sock, msg, MSG_LEN, 0, (struct sockaddr *)&s->client_addr,
         sizeof(s->client_addr));
}

/**
 * @brief Transition to IDLE state and clear client.
 */
static void transition_to_idle(struct udp_streamer *s, const char *reason) {
  if (s->state == STREAM_IDLE) {
    return;
  }
  s->state = STREAM_IDLE;
  ESP_LOGW(TAG, "-> IDLE (%s)", reason);
}

/**
 * @brief Background task that listens for control messages and checks
 * heartbeat.
 */
static void control_listener_task(void *arg) {
  struct udp_streamer *s = (struct udp_streamer *)arg;
  char buf[64];
  uint32_t poll_count = 0;

  ESP_LOGI(TAG, "Control listener waiting on port %d (sock=%d)...",
           VIDEO_UDP_PORT, s->sock);

  while (true) {
    struct sockaddr_in src_addr = {0};
    socklen_t addr_len = sizeof(src_addr);

    int len = recvfrom(s->sock, buf, sizeof(buf) - 1, 0,
                       (struct sockaddr *)&src_addr, &addr_len);

    /* Check heartbeat timeout on every wake (including recvfrom timeout) */
    if (s->state != STREAM_IDLE && s->last_heartbeat_us > 0) {
      int64_t elapsed = esp_timer_get_time() - s->last_heartbeat_us;
      if (elapsed > HEARTBEAT_TIMEOUT_US) {
        transition_to_idle(s, "heartbeat timeout");
      }
    }

    if (len < 0) {
      if (errno == EBADF) {
        break;
      }
      poll_count++;
      if (poll_count % 5 == 0) {
        ESP_LOGI(TAG, "Listener: waiting (polls=%lu, state=%d)",
                 (unsigned long)poll_count, (int)s->state);
      }
      continue;
    }

    /* Log received message */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntoa_r(src_addr.sin_addr, ip_str, sizeof(ip_str));

    if (len < MSG_LEN) {
      ESP_LOGW(TAG, "Short packet (%d bytes) from %s:%d", len, ip_str,
               ntohs(src_addr.sin_port));
      continue;
    }

    /* Dispatch on 4-byte message type */
    if (memcmp(buf, MSG_VID0, MSG_LEN) == 0) {
      memcpy(&s->client_addr, &src_addr, sizeof(src_addr));
      s->last_heartbeat_us = esp_timer_get_time();
      s->force_idr = true;
      s->state = STREAM_ACTIVE;
      send_control(s, MSG_VACK);
      ESP_LOGI(TAG, "-> ACTIVE (VID0 from %s:%d)", ip_str,
               ntohs(src_addr.sin_port));

    } else if (memcmp(buf, MSG_BEAT, MSG_LEN) == 0) {
      if (s->state == STREAM_IDLE) {
        /* Re-register on BEAT in IDLE — app may have resumed after timeout */
        memcpy(&s->client_addr, &src_addr, sizeof(src_addr));
        s->force_idr = true;
        s->state = STREAM_ACTIVE;
        send_control(s, MSG_VACK);
        ESP_LOGI(TAG, "-> ACTIVE (BEAT re-registered %s:%d)", ip_str,
                 ntohs(src_addr.sin_port));
      }
      s->last_heartbeat_us = esp_timer_get_time();
      if (s->state == STREAM_PAUSED) {
        s->force_idr = true;
        s->state = STREAM_ACTIVE;
        ESP_LOGI(TAG, "-> ACTIVE (resumed via BEAT)");
      }

    } else if (memcmp(buf, MSG_PAWS, MSG_LEN) == 0) {
      if (s->state == STREAM_ACTIVE) {
        s->state = STREAM_PAUSED;
        ESP_LOGI(TAG, "-> PAUSED (app backgrounded)");
      }

    } else if (memcmp(buf, MSG_GONE, MSG_LEN) == 0) {
      transition_to_idle(s, "client sent GONE");

    } else {
      ESP_LOGW(TAG, "Unknown message [%02x %02x %02x %02x] from %s:%d",
               (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2],
               (uint8_t)buf[3], ip_str, ntohs(src_addr.sin_port));
    }
  }

  vTaskDelete(NULL);
}

esp_err_t udp_stream_start(udp_streamer_handle_t *out) {
  struct udp_streamer *s = calloc(1, sizeof(*s));
  if (s == NULL) {
    return ESP_ERR_NO_MEM;
  }

  s->state = STREAM_IDLE;
  s->last_heartbeat_us = 0;
  s->force_idr = false;

  s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s->sock < 0) {
    ESP_LOGE(TAG, "Socket creation failed: %s", strerror(errno));
    free(s);
    return ESP_FAIL;
  }

  /* Allow address reuse and increase receive buffer */
  int reuse = 1;
  setsockopt(s->sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  int rcvbuf = 32768;
  setsockopt(s->sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(VIDEO_UDP_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(s->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "Bind failed on port %d: %s", VIDEO_UDP_PORT,
             strerror(errno));
    close(s->sock);
    free(s);
    return ESP_FAIL;
  }

  /* 2s receive timeout — listener wakes periodically to check heartbeat */
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Spawn control listener in background */
  BaseType_t ret =
      xTaskCreatePinnedToCore(control_listener_task, "ctrl_listen",
                              LISTENER_STACK, s, 5, &s->listener_task, 1);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create control listener task");
    close(s->sock);
    free(s);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "UDP streamer started on port %d (waiting for VID0)",
           VIDEO_UDP_PORT);
  *out = s;
  return ESP_OK;
}

esp_err_t udp_stream_send_frame(udp_streamer_handle_t streamer,
                                const uint8_t *h264_data, uint32_t h264_size,
                                uint32_t frame_id, uint16_t width,
                                uint16_t height, bool is_keyframe) {
  if (streamer->state != STREAM_ACTIVE) {
    return ESP_ERR_NOT_FOUND;
  }

  uint16_t chunk_count = (h264_size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
  uint32_t timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

  /* Scratch buffer: header + payload in one sendto call */
  uint8_t pkt[sizeof(h264_chunk_header_t) + MAX_PAYLOAD_SIZE];
  h264_chunk_header_t *hdr = (h264_chunk_header_t *)pkt;

  uint32_t offset = 0;
  for (uint16_t i = 0; i < chunk_count; i++) {
    uint32_t payload_len = h264_size - offset;
    if (payload_len > MAX_PAYLOAD_SIZE) {
      payload_len = MAX_PAYLOAD_SIZE;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->frame_id = frame_id;
    hdr->width = width;
    hdr->height = height;
    hdr->timestamp = timestamp_ms;
    hdr->total_len = h264_size;
    hdr->chunk_idx = i;
    hdr->chunk_count = chunk_count;
    hdr->frame_type = is_keyframe ? 1 : 0;

    memcpy(pkt + sizeof(h264_chunk_header_t), h264_data + offset, payload_len);

    int sent =
        sendto(streamer->sock, pkt, sizeof(h264_chunk_header_t) + payload_len,
               0, (struct sockaddr *)&streamer->client_addr,
               sizeof(streamer->client_addr));
    if (sent < 0) {
      ESP_LOGW(TAG, "sendto failed: %s", strerror(errno));
      return ESP_FAIL;
    }

    offset += payload_len;

    /* Pace chunks to avoid flooding Wi-Fi TX queue */
    if (i + 1 < chunk_count) {
      usleep(200);
    }
  }

  return ESP_OK;
}

udp_stream_state_t udp_stream_get_state(udp_streamer_handle_t streamer) {
  if (streamer == NULL) {
    return STREAM_IDLE;
  }
  return streamer->state;
}

bool udp_stream_should_force_idr(udp_streamer_handle_t streamer) {
  if (streamer == NULL || !streamer->force_idr) {
    return false;
  }
  streamer->force_idr = false;
  return true;
}

void udp_stream_on_station_disconnect(udp_streamer_handle_t streamer) {
  if (streamer == NULL) {
    return;
  }
  transition_to_idle(streamer, "station disconnected");
}

bool udp_stream_get_client_addr(udp_streamer_handle_t streamer,
                                struct sockaddr_in *out_addr) {
  if (streamer == NULL || out_addr == NULL) {
    return false;
  }
  if (streamer->state == STREAM_IDLE) {
    return false;
  }
  memcpy(out_addr, &streamer->client_addr, sizeof(*out_addr));
  return true;
}

void udp_stream_stop(udp_streamer_handle_t streamer) {
  if (streamer == NULL) {
    return;
  }

  /* Close socket first — this unblocks the recvfrom in the listener */
  if (streamer->sock >= 0) {
    close(streamer->sock);
    streamer->sock = -1;
  }

  /* Give the listener task time to exit */
  vTaskDelay(pdMS_TO_TICKS(200));

  free(streamer);
  ESP_LOGI(TAG, "UDP streamer stopped");
}
