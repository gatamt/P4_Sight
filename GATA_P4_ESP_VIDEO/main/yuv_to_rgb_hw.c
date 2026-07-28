/**
 * @file yuv_to_rgb_hw.c
 * @brief Packed O_UYY_E_VYY to RGB888 decoder for ESP32-P4 ISP output.
 *
 * The ESP32-P4 ISP's "YUV420" is the proprietary O_UYY_E_VYY packed format:
 *
 *   Even rows (0, 2, 4 …): U  Y  Y │ U  Y  Y │ …   (3 bytes per 2 pixels)
 *   Odd  rows (1, 3, 5 …): V  Y  Y │ V  Y  Y │ …
 *
 *   Row stride  = width × 3 / 2     (1920 bytes for 1280-wide frame)
 *   Total frame = stride × height   (1 382 400 bytes for 1280×720)
 *
 * Chroma is shared between even/odd row pairs (420 subsampling).
 * For pixel (x, y):
 *   triplet = x / 2
 *   Y = current_row [ triplet × 3 + 1 + (x & 1) ]
 *   U = even_row     [ triplet × 3 ]       (row  y & ~1)
 *   V = odd_row      [ triplet × 3 ]       (row  y |  1)
 *
 * Conversion uses full-range BT.601 coefficients.
 * Decimation is integrated: only the target pixels are decoded.
 */

#include "yuv_to_rgb_hw.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "yuv-ouyy";

static bool s_initialised = false;

/* ── Clamp helper ────────────────────────────────────────────────────── */

static inline uint8_t clamp8(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return (uint8_t)v;
}

/* Public API. The file name is retained for compatibility; conversion is software-based. */

esp_err_t yuv_to_rgb_hw_init(void) {
  if (!s_initialised) {
    ESP_LOGI(TAG, "O_UYY_E_VYY decoder ready (pure software, no HW deps)");
    s_initialised = true;
  }
  return ESP_OK;
}

esp_err_t yuv_to_rgb_hw_alloc_buf(uint32_t width, uint32_t height,
                                  uint8_t **out_buf, uint32_t *out_size) {
  if (out_buf == NULL || out_size == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const uint32_t size = width * height * 3;
  uint8_t *buf = (uint8_t *)heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (buf == NULL) {
    ESP_LOGE(TAG, "alloc_buf %lux%lu (%lu bytes) failed", (unsigned long)width,
             (unsigned long)height, (unsigned long)size);
    return ESP_ERR_NO_MEM;
  }

  *out_buf = buf;
  *out_size = size;
  return ESP_OK;
}

esp_err_t yuv_to_rgb_hw_convert(const uint8_t *yuv_data, uint32_t src_w,
                                uint32_t src_h, uint8_t *rgb_out,
                                uint32_t dst_w, uint32_t dst_h) {
  if (yuv_data == NULL || rgb_out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  /*
   * O_UYY_E_VYY packed layout:
   *   stride = src_w * 3 / 2
   *   Even rows carry V, odd rows carry U.
   *   Each triplet = [chroma, Y_left, Y_right]
   */
  const uint32_t stride = src_w * 3 / 2;

  for (uint32_t dy = 0; dy < dst_h; dy++) {
    const uint32_t sy = dy * src_h / dst_h;

    /* Pointers to the even/odd row pair that shares chroma. */
    const uint8_t *u_row = yuv_data + (sy & ~1u) * stride; /* even → U */
    const uint8_t *v_row = yuv_data + (sy | 1u) * stride;  /* odd  → V */
    const uint8_t *y_row = yuv_data + sy * stride;         /* current */

    uint8_t *dp = rgb_out + dy * dst_w * 3;

    for (uint32_t dx = 0; dx < dst_w; dx++) {
      const uint32_t sx = dx * src_w / dst_w;
      const uint32_t triplet = sx / 2;
      const uint32_t t3 = triplet * 3;

      /* Extract Y, U, V from packed positions. */
      const int y_val = y_row[t3 + 1 + (sx & 1)];
      const int u_val = u_row[t3];
      const int v_val = v_row[t3];

      /* BT.601 full-range → RGB. */
      const int cb = u_val - 128;
      const int cr = v_val - 128;

      dp[0] = clamp8((256 * y_val + 359 * cr + 128) >> 8);           /* R */
      dp[1] = clamp8((256 * y_val - 88 * cb - 183 * cr + 128) >> 8); /* G */
      dp[2] = clamp8((256 * y_val + 454 * cb + 128) >> 8);           /* B */
      dp += 3;
    }
  }

  return ESP_OK;
}

void yuv_to_rgb_hw_deinit(void) { s_initialised = false; }
