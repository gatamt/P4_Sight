/**
 * @file yuv_to_rgb.c
 * @brief NV12 semi-planar to RGB888 with nearest-neighbor decimation.
 *
 * ESP32-P4 ISP outputs NV12 (Y plane + interleaved UV plane) despite
 * V4L2 reporting the format as YU12. The UV bytes are interleaved as
 * U0,V0, U1,V1, U2,V2, ... with row stride equal to source width.
 */

#include "yuv_to_rgb.h"

/** @brief Clamp int to [0, 255]. */
static inline uint8_t clamp_u8(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return (uint8_t)v;
}

void yuv420_to_rgb888_decimate(const uint8_t *yuv_data, uint32_t src_w,
                               uint32_t src_h, uint8_t *rgb_out, uint32_t dst_w,
                               uint32_t dst_h) {
  const uint32_t y_plane_size = src_w * src_h;

  const uint8_t *y_plane = yuv_data;
  /* NV12: UV plane starts after Y, interleaved as U,V,U,V,...
   * Row stride = src_w bytes (src_w/2 UV pairs × 2 bytes each) */
  const uint8_t *uv_plane = yuv_data + y_plane_size;

  uint32_t out_idx = 0;

  for (uint32_t dy = 0; dy < dst_h; dy++) {
    uint32_t sy = (dy * src_h) / dst_h;

    for (uint32_t dx = 0; dx < dst_w; dx++) {
      uint32_t sx = (dx * src_w) / dst_w;

      /* Y from luma plane */
      uint8_t y_val = y_plane[sy * src_w + sx];

      /* UV from interleaved NV12 chroma plane */
      uint32_t uv_offset = (sy / 2) * src_w + (sx & ~1u);
      uint8_t u_val = uv_plane[uv_offset];
      uint8_t v_val = uv_plane[uv_offset + 1];

      /* BT.601 limited-range YUV -> RGB */
      int c = (int)y_val - 16;
      int d = (int)u_val - 128;
      int e = (int)v_val - 128;

      int r = (298 * c + 409 * e + 128) >> 8;
      int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
      int b = (298 * c + 516 * d + 128) >> 8;

      rgb_out[out_idx++] = clamp_u8(r);
      rgb_out[out_idx++] = clamp_u8(g);
      rgb_out[out_idx++] = clamp_u8(b);
    }
  }
}
