/**
 * @file ai_task_stub.c
 * @brief Stub for ai_task when ESP-DL is not linked (diagnostic builds only).
 */

#include "ai_task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ai-stub";
static ai_result_buffer_t s_empty_results;

esp_err_t ai_task_start(udp_streamer_handle_t streamer) {
  ESP_LOGW(TAG, "AI task STUB — ESP-DL not linked, no inference");
  memset(&s_empty_results, 0, sizeof(s_empty_results));
  return ESP_OK;
}

const ai_result_buffer_t *ai_task_get_results(void) { return &s_empty_results; }
