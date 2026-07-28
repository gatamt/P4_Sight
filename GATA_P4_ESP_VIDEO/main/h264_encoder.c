/**
 * @file h264_encoder.c
 * @brief H.264 M2M encoder via V4L2 /dev/video11.
 *
 * Follows the same M2M pattern as the esp_video UVC example:
 * - Open /dev/video11
 * - Set OUTPUT format (YUV420 input) and CAPTURE format (H.264 output)
 * - REQBUFS + STREAMON on both sides
 * - Per-frame: QBUF output (USERPTR) → DQBUF capture → read encoded data
 */

#include "h264_encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_log.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

static const char *TAG = "h264-enc";

/** GOP and quality — tuned for Wi-Fi UDP streaming. */
#define H264_GOP 30
#define H264_BITRATE 800000
#define H264_MIN_QP 25
#define H264_MAX_QP 40

struct h264_enc {
  int fd;
  uint32_t width;
  uint32_t height;
  uint8_t *cap_buffer;
  uint32_t cap_buf_size;
  uint32_t frame_count;
};

/**
 * @brief Set a single V4L2 codec extended control.
 */
static esp_err_t set_codec_ctrl(int fd, uint32_t ctrl_id, int32_t value) {
  struct v4l2_ext_control ctrl = {0};
  struct v4l2_ext_controls ctrls = {0};

  ctrl.id = ctrl_id;
  ctrl.value = value;
  ctrls.ctrl_class = V4L2_CID_CODEC_CLASS;
  ctrls.count = 1;
  ctrls.controls = &ctrl;

  if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
    ESP_LOGW(TAG, "Failed to set codec ctrl 0x%lx=%ld: %s",
             (unsigned long)ctrl_id, (long)value, strerror(errno));
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t h264_enc_open(uint32_t width, uint32_t height,
                        h264_enc_handle_t *out) {
  struct h264_enc *enc = calloc(1, sizeof(*enc));
  if (enc == NULL) {
    return ESP_ERR_NO_MEM;
  }
  enc->fd = -1;
  enc->width = width;
  enc->height = height;

  /* Open H.264 M2M device */
  enc->fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDWR);
  if (enc->fd < 0) {
    ESP_LOGE(TAG, "Failed to open %s: %s", ESP_VIDEO_H264_DEVICE_NAME,
             strerror(errno));
    free(enc);
    return ESP_FAIL;
  }

  /* Configure encoder parameters */
  set_codec_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, H264_GOP);
  set_codec_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE, H264_BITRATE);
  set_codec_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, H264_MIN_QP);
  set_codec_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, H264_MAX_QP);

  /* OUTPUT side: YUV420 input from camera */
  struct v4l2_format out_fmt = {0};
  out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out_fmt.fmt.pix.width = width;
  out_fmt.fmt.pix.height = height;
  out_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  if (ioctl(enc->fd, VIDIOC_S_FMT, &out_fmt) != 0) {
    ESP_LOGE(TAG, "S_FMT OUTPUT failed: %s", strerror(errno));
    goto fail;
  }

  struct v4l2_requestbuffers out_req = {0};
  out_req.count = 1;
  out_req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out_req.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(enc->fd, VIDIOC_REQBUFS, &out_req) != 0) {
    ESP_LOGE(TAG, "REQBUFS OUTPUT failed: %s", strerror(errno));
    goto fail;
  }

  /* CAPTURE side: H.264 output */
  struct v4l2_format cap_fmt = {0};
  cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_fmt.fmt.pix.width = width;
  cap_fmt.fmt.pix.height = height;
  cap_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
  if (ioctl(enc->fd, VIDIOC_S_FMT, &cap_fmt) != 0) {
    ESP_LOGE(TAG, "S_FMT CAPTURE failed: %s", strerror(errno));
    goto fail;
  }

  struct v4l2_requestbuffers cap_req = {0};
  cap_req.count = 1;
  cap_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(enc->fd, VIDIOC_REQBUFS, &cap_req) != 0) {
    ESP_LOGE(TAG, "REQBUFS CAPTURE failed: %s", strerror(errno));
    goto fail;
  }

  /* Map the capture buffer */
  struct v4l2_buffer cap_buf = {0};
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  cap_buf.index = 0;
  if (ioctl(enc->fd, VIDIOC_QUERYBUF, &cap_buf) != 0) {
    ESP_LOGE(TAG, "QUERYBUF CAPTURE failed: %s", strerror(errno));
    goto fail;
  }

  enc->cap_buf_size = cap_buf.length;
  enc->cap_buffer = mmap(NULL, cap_buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, enc->fd, cap_buf.m.offset);
  if (enc->cap_buffer == MAP_FAILED) {
    ESP_LOGE(TAG, "mmap CAPTURE failed: %s", strerror(errno));
    enc->cap_buffer = NULL;
    goto fail;
  }

  /* Queue the capture buffer and start both streams */
  if (ioctl(enc->fd, VIDIOC_QBUF, &cap_buf) != 0) {
    ESP_LOGE(TAG, "QBUF CAPTURE failed: %s", strerror(errno));
    goto fail;
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(enc->fd, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "STREAMON CAPTURE failed: %s", strerror(errno));
    goto fail;
  }
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(enc->fd, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "STREAMON OUTPUT failed: %s", strerror(errno));
    goto fail;
  }

  ESP_LOGI(TAG, "H.264 encoder opened: %lux%lu GOP=%d bitrate=%d",
           (unsigned long)width, (unsigned long)height, H264_GOP, H264_BITRATE);

  *out = enc;
  return ESP_OK;

fail:
  if (enc->cap_buffer && enc->cap_buffer != MAP_FAILED) {
    munmap(enc->cap_buffer, enc->cap_buf_size);
  }
  if (enc->fd >= 0) {
    close(enc->fd);
  }
  free(enc);
  return ESP_FAIL;
}

esp_err_t h264_enc_process(h264_enc_handle_t enc, const uint8_t *yuv_data,
                           uint32_t yuv_size, uint8_t **out_data,
                           uint32_t *out_size, bool *is_keyframe) {
  /* Submit YUV frame as USERPTR on OUTPUT side */
  struct v4l2_buffer out_buf = {0};
  out_buf.index = 0;
  out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out_buf.memory = V4L2_MEMORY_USERPTR;
  out_buf.m.userptr = (unsigned long)yuv_data;
  out_buf.length = yuv_size;
  if (ioctl(enc->fd, VIDIOC_QBUF, &out_buf) != 0) {
    ESP_LOGE(TAG, "QBUF OUTPUT failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  /* Dequeue encoded H.264 from CAPTURE side */
  struct v4l2_buffer cap_buf = {0};
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  if (ioctl(enc->fd, VIDIOC_DQBUF, &cap_buf) != 0) {
    ESP_LOGE(TAG, "DQBUF CAPTURE failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  /* Dequeue the consumed OUTPUT buffer */
  struct v4l2_buffer out_done = {0};
  out_done.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out_done.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(enc->fd, VIDIOC_DQBUF, &out_done) != 0) {
    ESP_LOGW(TAG, "DQBUF OUTPUT failed: %s", strerror(errno));
  }

  *out_data = enc->cap_buffer;
  *out_size = cap_buf.bytesused;

  /* Keyframe detection: IDR frames have V4L2_BUF_FLAG_KEYFRAME,
     or we check GOP counter as fallback. */
  bool kf = (cap_buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
  if (!kf) {
    kf = (enc->frame_count % H264_GOP) == 0;
  }
  *is_keyframe = kf;
  enc->frame_count++;

  /* Re-queue the capture buffer for the next frame */
  struct v4l2_buffer requeue = {0};
  requeue.index = 0;
  requeue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  requeue.memory = V4L2_MEMORY_MMAP;
  if (ioctl(enc->fd, VIDIOC_QBUF, &requeue) != 0) {
    ESP_LOGW(TAG, "Re-QBUF CAPTURE failed: %s", strerror(errno));
  }

  return ESP_OK;
}

void h264_enc_force_idr(h264_enc_handle_t enc) {
  if (enc == NULL || enc->fd < 0) {
    return;
  }
  esp_err_t ret =
      set_codec_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Forced IDR on next frame");
  }
}

void h264_enc_close(h264_enc_handle_t enc) {
  if (enc == NULL) {
    return;
  }

  int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ioctl(enc->fd, VIDIOC_STREAMOFF, &type);
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(enc->fd, VIDIOC_STREAMOFF, &type);

  if (enc->cap_buffer && enc->cap_buffer != MAP_FAILED) {
    munmap(enc->cap_buffer, enc->cap_buf_size);
  }

  if (enc->fd >= 0) {
    close(enc->fd);
  }

  free(enc);
  ESP_LOGI(TAG, "H.264 encoder closed");
}
