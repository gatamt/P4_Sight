#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_cam_sensor_types.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imx708_types.h"
#include "linux/videodev2.h"

/* Streaming and inference modules. */
#include "ai_task.h"
#include "h264_encoder.h"
#include "mdns_announce.h"
#include "soc/dw_gdma_struct.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/mipi_csi_host_struct.h"
#include "udp_stream.h"
#include "wifi_station.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define CAM_I2C_PORT I2C_NUM_0
#define CAM_I2C_SDA GPIO_NUM_7
#define CAM_I2C_SCL GPIO_NUM_8
#define CAM_I2C_FREQ_HZ 400000
#define CAM_I2C_ADDR 0x1A

#define CAM_MIPI_LDO_CHANNEL 3
#define CAM_MIPI_LDO_MV 2500
#define LDO_OFF_TIME_MS 2000
#define LDO_STABLE_TIME_MS 1000
#define LP11_WAIT_MS 250
#define IMX708_RESET_SETTLE_MS 150
#define PREEMPTIVE_RESET_MS 500

#define CAPTURE_BUFFER_COUNT 3
#define CSI_AFULL_THRESHOLD 2040
#define FAST_FIRST_FRAME_TIMEOUT_MS 500
#define RETRY_FIRST_FRAME_TIMEOUT_MS 3000
#define MIPI_LINK_MAX_RETRIES 3
#define MIPI_LIGHT_RETRIES 0
#define SMOKE_FRAME_COUNT 5
#define IMX708_REG_STREAMING 0x0100
#define IMX708_REG_RESET 0x0103

typedef struct {
  i2c_master_bus_handle_t i2c_bus;
  esp_ldo_channel_handle_t ldo_handle;
  int cap_fd;
  bool stream_on;
  void *buffers[CAPTURE_BUFFER_COUNT];
  size_t buffer_lengths[CAPTURE_BUFFER_COUNT];
} bringup_state_t;

typedef struct {
  int fd;
  SemaphoreHandle_t done_sem;
  bool success;
  uint32_t bytesused;
} dqbuf_test_t;

static const char *TAG = "p4-imx708";

extern volatile uint32_t csi_isr_trans_finished_cnt;
extern volatile uint32_t csi_isr_get_new_trans_cnt;
extern volatile uint32_t csi_isr_frame_dropped_cnt;
extern volatile uint32_t csi_isr_queue_empty_cnt;

/** Latest captured YUV420 frame — written by capture loop, read by AI task.
 *  No lock: AI reads a stale pointer at worst (acceptable for detection). */
volatile const uint8_t *g_latest_yuv_frame = NULL;
volatile uint32_t g_latest_yuv_size = 0;
volatile uint32_t g_latest_frame_id = 0;

// Use scale-only mode. Hardware tests showed that crop mode 0x43 can stall on P4.
static const imx708_reginfo_t s_custom_imx708_720p_regs[] = {
    {0x0342, 0x31}, {0x0343, 0xC4}, {0x0340, 0x06}, {0x0341, 0x16},
    {0x0344, 0x00}, {0x0345, 0x00}, {0x0346, 0x00}, {0x0347, 0x00},
    {0x0348, 0x11}, {0x0349, 0xFF}, {0x034A, 0x0A}, {0x034B, 0x1F},
    {0x0220, 0x62}, {0x0222, 0x01}, {0x0900, 0x01}, {0x0901, 0x22},
    {0x0902, 0x08}, {0x3200, 0x41}, {0x3201, 0x41}, {0x32D5, 0x00},
    {0x32D6, 0x00}, {0x32DB, 0x01}, {0x32DF, 0x00}, {0x350C, 0x00},
    {0x350D, 0x00}, {0x0408, 0x02}, {0x0409, 0x00}, {0x040A, 0x01},
    {0x040B, 0x20}, {0x040C, 0x05}, {0x040D, 0x00}, {0x040E, 0x02},
    {0x040F, 0xD0}, {0x034C, 0x05}, {0x034D, 0x00}, {0x034E, 0x02},
    {0x034F, 0xD0}, {0x0301, 0x05}, {0x0303, 0x02}, {0x0305, 0x02},
    {0x0306, 0x00}, {0x0307, 0x7C}, {0x030B, 0x02}, {0x030D, 0x04},
    {0x0310, 0x01}, {0x3CA0, 0x00}, {0x3CA1, 0x3C}, {0x3CA4, 0x00},
    {0x3CA5, 0x3C}, {0x3CA6, 0x00}, {0x3CA7, 0x00}, {0x3CAA, 0x00},
    {0x3CAB, 0x00}, {0x3CB8, 0x00}, {0x3CB9, 0x1C}, {0x3CBA, 0x00},
    {0x3CBB, 0x08}, {0x3CBC, 0x00}, {0x3CBD, 0x1E}, {0x3CBE, 0x00},
    {0x3CBF, 0x0A}, {0x0202, 0x05}, {0x0203, 0xAC}, {0x0204, 0x00},
    {0x0205, 0x00}, {0x020E, 0x01}, {0x020F, 0x00}, {0x0224, 0x01},
    {0x0225, 0xF4}, {0x3116, 0x01}, {0x3117, 0xF4}, {0x0216, 0x00},
    {0x0217, 0x00}, {0x0218, 0x01}, {0x0219, 0x00}, {0x3118, 0x00},
    {0x3119, 0x00}, {0x311A, 0x01}, {0x311B, 0x00}, {0x341A, 0x00},
    {0x341B, 0x00}, {0x341C, 0x00}, {0x341D, 0x00}, {0x341E, 0x00},
    {0x341F, 0x50}, {0x3420, 0x00}, {0x3421, 0x3C}, {0x3366, 0x00},
    {0x3367, 0x00}, {0x3368, 0x00}, {0x3369, 0x00}, {0x0101, 0x03},
    {0xFFFF, 0x00},
};

static const esp_cam_sensor_isp_info_t s_custom_isp_info = {
    .isp_v1_info =
        {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = 182400000,
            .vts = 1558,
            .hts = 12740,
            .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
        },
};

static const esp_cam_sensor_format_t s_custom_720p_format = {
    .name = "MIPI_2lane_24Minput_RAW10_1280x720_scale_only",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1280,
    .height = 720,
    .regs = s_custom_imx708_720p_regs,
    .regs_size = ARRAY_SIZE(s_custom_imx708_720p_regs),
    .fps = 30,
    .isp_info = &s_custom_isp_info,
    .mipi_info =
        {
            .mipi_clk = 450000000,
            .lane_num = 2,
            .line_sync_en = false,
        },
    .reserved = NULL,
};

static void dump_csi_state(const char *stage) {
  ESP_LOGW(TAG,
           "[%s] BRG: en=%d clk_en=%d cfg_clk_en=%d buf_depth=%lu afull=%lu "
           "raw=0x%08lx",
           stage, (int)MIPI_CSI_BRIDGE.csi_en.csi_brg_en,
           (int)MIPI_CSI_BRIDGE.host_ctrl.csi_enableclk,
           (int)MIPI_CSI_BRIDGE.host_ctrl.csi_cfg_clk_en,
           (unsigned long)MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth,
           (unsigned long)MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_afull_thrd,
           (unsigned long)MIPI_CSI_BRIDGE.int_raw.val);
  ESP_LOGW(TAG,
           "[%s] BRG errors: vadr_gt=%d vadr_lt=%d discard=%d overrun=%d "
           "fifo_ovf=%d dma_upd=%d",
           stage, (int)MIPI_CSI_BRIDGE.int_raw.vadr_num_gt_int_raw,
           (int)MIPI_CSI_BRIDGE.int_raw.vadr_num_lt_int_raw,
           (int)MIPI_CSI_BRIDGE.int_raw.discard_int_raw,
           (int)MIPI_CSI_BRIDGE.int_raw.csi_buf_overrun_int_raw,
           (int)MIPI_CSI_BRIDGE.int_raw.csi_async_fifo_ovf_int_raw,
           (int)MIPI_CSI_BRIDGE.int_raw.dma_cfg_has_updated_int_raw);
  ESP_LOGW(TAG,
           "[%s] HOST: skipped direct MIPI_CSI_HOST register reads on P4 v1.0 "
           "to avoid load faults",
           stage);
  ESP_LOGW(TAG,
           "[%s] ISP: skipped direct ISP/GDMA register reads in app-level "
           "diagnostics",
           stage);
  ESP_LOGW(TAG, "[%s] ISR counters: fin=%lu new=%lu drop=%lu q_empty=%lu",
           stage, (unsigned long)csi_isr_trans_finished_cnt,
           (unsigned long)csi_isr_get_new_trans_cnt,
           (unsigned long)csi_isr_frame_dropped_cnt,
           (unsigned long)csi_isr_queue_empty_cnt);
}

static void reset_csi_isr_counters(void) {
  csi_isr_trans_finished_cnt = 0;
  csi_isr_get_new_trans_cnt = 0;
  csi_isr_frame_dropped_cnt = 0;
  csi_isr_queue_empty_cnt = 0;
}

static void dqbuf_test_task(void *arg) {
  dqbuf_test_t *test = (dqbuf_test_t *)arg;
  struct v4l2_buffer buf = {
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
      .memory = V4L2_MEMORY_MMAP,
  };

  if (ioctl(test->fd, VIDIOC_DQBUF, &buf) == 0) {
    test->success = true;
    test->bytesused = buf.bytesused;
    ioctl(test->fd, VIDIOC_QBUF, &buf);
  }

  xSemaphoreGive(test->done_sem);
  vTaskDelete(NULL);
}

static bool try_dqbuf_with_timeout(int fd, uint32_t timeout_ms,
                                   uint32_t *out_bytesused) {
  dqbuf_test_t test = {
      .fd = fd,
      .done_sem = xSemaphoreCreateBinary(),
      .success = false,
      .bytesused = 0,
  };
  TaskHandle_t task_h = NULL;

  if (test.done_sem == NULL) {
    return false;
  }

  if (xTaskCreatePinnedToCore(dqbuf_test_task, "dqtest", 4096, &test, 5,
                              &task_h, 0) != pdPASS) {
    vSemaphoreDelete(test.done_sem);
    return false;
  }

  bool got_frame =
      (xSemaphoreTake(test.done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) &&
      test.success;
  if (!got_frame && task_h != NULL) {
    vTaskDelete(task_h);
  }
  vSemaphoreDelete(test.done_sem);

  if (got_frame && out_bytesused) {
    *out_bytesused = test.bytesused;
  }
  return got_frame;
}

static esp_err_t power_cycle_camera_ldo(bringup_state_t *state) {
  esp_ldo_channel_config_t ldo_cfg = {
      .chan_id = CAM_MIPI_LDO_CHANNEL,
      .voltage_mv = CAM_MIPI_LDO_MV,
  };

  if (!state->ldo_handle) {
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &state->ldo_handle),
                        TAG, "LDO acquire failed");
  }

  ESP_LOGI(TAG, "Power-cycling camera LDO3 for a hard IMX708 reset");
  ESP_RETURN_ON_ERROR(esp_ldo_release_channel(state->ldo_handle), TAG,
                      "LDO release failed");
  state->ldo_handle = NULL;
  vTaskDelay(pdMS_TO_TICKS(LDO_OFF_TIME_MS));
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &state->ldo_handle),
                      TAG, "LDO re-acquire failed");
  vTaskDelay(pdMS_TO_TICKS(LDO_STABLE_TIME_MS));
  return ESP_OK;
}

static esp_err_t init_i2c(bringup_state_t *state) {
  if (state->i2c_bus) {
    return ESP_OK;
  }

  i2c_master_bus_config_t cfg = {
      .i2c_port = CAM_I2C_PORT,
      .sda_io_num = CAM_I2C_SDA,
      .scl_io_num = CAM_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };

  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &state->i2c_bus), TAG,
                      "i2c init failed");
  ESP_LOGI(TAG, "I2C ready on GPIO%d/GPIO%d @ %dHz", CAM_I2C_SDA, CAM_I2C_SCL,
           CAM_I2C_FREQ_HZ);
  return ESP_OK;
}

static esp_err_t init_video_stack(bringup_state_t *state) {
  esp_video_init_csi_config_t csi_cfg = {
      .sccb_config =
          {
              .init_sccb = false,
              .i2c_handle = state->i2c_bus,
              .freq = CAM_I2C_FREQ_HZ,
          },
      .reset_pin = -1,
      .pwdn_pin = -1,
  };
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
  esp_video_init_cam_motor_config_t motor_cfg = {
      .sccb_config =
          {
              .init_sccb = false,
              .i2c_handle = state->i2c_bus,
              .freq = CAM_I2C_FREQ_HZ,
          },
      .reset_pin = -1,
      .pwdn_pin = -1,
      .signal_pin = -1,
  };
#endif
  esp_video_init_config_t video_cfg = {
      .csi = &csi_cfg,
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
      .cam_motor = &motor_cfg,
#endif
  };

  ESP_RETURN_ON_ERROR(esp_video_init(&video_cfg), TAG, "esp_video_init failed");
  ESP_LOGI(TAG, "esp_video initialized");
  return ESP_OK;
}

static esp_err_t set_isp_ctrl(int isp_fd, uint32_t ctrl_id, const void *data,
                              size_t size) {
  struct v4l2_ext_control ctrl = {0};
  struct v4l2_ext_controls ctrls = {0};

  ctrl.id = ctrl_id;
  ctrl.size = size;
  ctrl.p_u8 = (uint8_t *)data;
  ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
  ctrls.count = 1;
  ctrls.controls = &ctrl;

  return ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t configure_isp(void) {
  int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
  if (isp_fd < 0) {
    ESP_LOGE(TAG, "Failed to open %s: %s", ESP_VIDEO_ISP1_DEVICE_NAME,
             strerror(errno));
    return ESP_FAIL;
  }

  esp_video_isp_wb_t wb = {
      .enable = true,
      .red_gain = 1.0f,
      .blue_gain = 1.0f,
  };
  esp_video_isp_ccm_t ccm = {
      .enable = true,
      .matrix =
          {
              {1.0f, 0.0f, 0.0f},
              {0.0f, 1.0f, 0.0f},
              {0.0f, 0.0f, 1.0f},
          },
  };
  esp_video_isp_demosaic_t demosaic = {
      .enable = true,
      .gradient_ratio = 1.0f,
  };
  esp_video_isp_gamma_t gamma = {
      .enable = false,
  };

  if (set_isp_ctrl(isp_fd, V4L2_CID_USER_ESP_ISP_WB, &wb, sizeof(wb)) !=
      ESP_OK) {
    ESP_LOGW(TAG, "ISP WB setup failed: %s", strerror(errno));
  }
  if (set_isp_ctrl(isp_fd, V4L2_CID_USER_ESP_ISP_CCM, &ccm, sizeof(ccm)) !=
      ESP_OK) {
    ESP_LOGW(TAG, "ISP CCM setup failed: %s", strerror(errno));
  }
  if (set_isp_ctrl(isp_fd, V4L2_CID_USER_ESP_ISP_DEMOSAIC, &demosaic,
                   sizeof(demosaic)) != ESP_OK) {
    ESP_LOGW(TAG, "ISP demosaic setup failed: %s", strerror(errno));
  }
  if (set_isp_ctrl(isp_fd, V4L2_CID_USER_ESP_ISP_GAMMA, &gamma,
                   sizeof(gamma)) != ESP_OK) {
    ESP_LOGW(TAG, "ISP gamma setup failed: %s", strerror(errno));
  }

  close(isp_fd);
  return ESP_OK;
}

static esp_err_t close_capture_pipeline(bringup_state_t *state) {
  if (state->cap_fd >= 0 && state->stream_on) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(state->cap_fd, VIDIOC_STREAMOFF, &type);
    state->stream_on = false;
  }

  if (state->cap_fd >= 0) {
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
      if (state->buffers[i] && state->buffers[i] != MAP_FAILED) {
        munmap(state->buffers[i], state->buffer_lengths[i]);
        state->buffers[i] = NULL;
        state->buffer_lengths[i] = 0;
      }
    }
    close(state->cap_fd);
    state->cap_fd = -1;
  }

  return ESP_OK;
}

static esp_err_t imx708_write_reg8(bringup_state_t *state, uint16_t reg,
                                   uint8_t value, const char *label) {
  i2c_device_config_t cam_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = CAM_I2C_ADDR,
      .scl_speed_hz = CAM_I2C_FREQ_HZ,
  };
  i2c_master_dev_handle_t cam_dev = NULL;

  ESP_RETURN_ON_ERROR(
      i2c_master_bus_add_device(state->i2c_bus, &cam_cfg, &cam_dev), TAG,
      "%s: i2c add device failed", label);

  uint8_t payload[] = {
      (uint8_t)(reg >> 8),
      (uint8_t)(reg & 0xFF),
      value,
  };
  esp_err_t ret = i2c_master_transmit(cam_dev, payload, sizeof(payload), 100);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s failed (0x%x), resetting I2C bus", label, ret);
    i2c_master_bus_reset(state->i2c_bus);
    vTaskDelay(pdMS_TO_TICKS(50));
    ret = i2c_master_transmit(cam_dev, payload, sizeof(payload), 100);
  }

  i2c_master_bus_rm_device(cam_dev);
  return ret;
}

static esp_err_t preemptive_camera_reset(bringup_state_t *state) {
  ESP_LOGI(TAG, "Pre-emptive IMX708 reset: force LP-11 before esp_video probes "
                "the sensor");

  esp_err_t ret = imx708_write_reg8(state, IMX708_REG_STREAMING, 0x00,
                                    "pre-reset stream-off");
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Pre-reset stream-off failed: 0x%x", ret);
  }
  vTaskDelay(pdMS_TO_TICKS(50));

  ret =
      imx708_write_reg8(state, IMX708_REG_RESET, 0x01, "sensor software reset");
  ESP_RETURN_ON_ERROR(ret, TAG, "pre-reset software reset failed");

  vTaskDelay(pdMS_TO_TICKS(PREEMPTIVE_RESET_MS));
  ESP_LOGI(TAG, "Camera silenced. MIPI lanes should now be idle.");
  return ESP_OK;
}

static esp_err_t select_capture_format(int fd, uint32_t width, uint32_t height,
                                       uint32_t *out_fmt) {
  struct v4l2_fmtdesc fmtdesc = {
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
  };

  *out_fmt = 0;
  for (uint32_t idx = 0;; idx++) {
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = idx;
    if (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
      break;
    }

    ESP_LOGI(TAG, "Capture fmt[%lu]: " V4L2_FMT_STR, (unsigned long)idx,
             V4L2_FMT_STR_ARG(fmtdesc.pixelformat));
    if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUV420) {
      *out_fmt = fmtdesc.pixelformat;
      break;
    }
  }

  if (*out_fmt == 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  struct v4l2_format fmt = {
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
  };
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = *out_fmt;
  fmt.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
  fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
  fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

  if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_FMT failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Capture format set to %lux%lu " V4L2_FMT_STR,
           (unsigned long)fmt.fmt.pix.width, (unsigned long)fmt.fmt.pix.height,
           V4L2_FMT_STR_ARG(fmt.fmt.pix.pixelformat));
  return ESP_OK;
}

static esp_err_t open_capture_pipeline(bringup_state_t *state) {
  state->cap_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (state->cap_fd < 0) {
    ESP_LOGE(TAG, "Failed to open %s: %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
             strerror(errno));
    return ESP_FAIL;
  }

  struct v4l2_capability cap = {0};
  if (ioctl(state->cap_fd, VIDIOC_QUERYCAP, &cap) != 0) {
    ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: %s", strerror(errno));
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Video card=%s driver=%s bus=%s", cap.card, cap.driver,
           cap.bus_info);

  if (ioctl(state->cap_fd, VIDIOC_S_SENSOR_FMT, &s_custom_720p_format) != 0) {
    ESP_LOGE(TAG, "VIDIOC_S_SENSOR_FMT failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  esp_cam_sensor_format_t readback = {0};
  if (ioctl(state->cap_fd, VIDIOC_G_SENSOR_FMT, &readback) == 0) {
    ESP_LOGI(TAG, "Sensor readback: %dx%d@%dfps name=%s", readback.width,
             readback.height, readback.fps,
             readback.name ? readback.name : "(null)");
  } else {
    ESP_LOGW(TAG, "VIDIOC_G_SENSOR_FMT failed: %s", strerror(errno));
  }

  uint32_t capture_fmt = 0;
  ESP_RETURN_ON_ERROR(
      select_capture_format(state->cap_fd, s_custom_720p_format.width,
                            s_custom_720p_format.height, &capture_fmt),
      TAG, "no usable YUV420 capture format found");

  struct v4l2_requestbuffers req = {
      .count = CAPTURE_BUFFER_COUNT,
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
      .memory = V4L2_MEMORY_MMAP,
  };
  if (ioctl(state->cap_fd, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: %s", strerror(errno));
    return ESP_FAIL;
  }

  for (uint32_t i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = i,
    };

    if (ioctl(state->cap_fd, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%lu) failed: %s", (unsigned long)i,
               strerror(errno));
      return ESP_FAIL;
    }

    state->buffer_lengths[i] = buf.length;
    state->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, state->cap_fd, buf.m.offset);
    if (state->buffers[i] == MAP_FAILED) {
      state->buffers[i] = NULL;
      ESP_LOGE(TAG, "mmap(%lu) failed: %s", (unsigned long)i, strerror(errno));
      return ESP_FAIL;
    }

    if (ioctl(state->cap_fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF(%lu) failed: %s", (unsigned long)i,
               strerror(errno));
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}

static esp_err_t sensor_stream_off_for_lp11(bringup_state_t *state) {
  ESP_RETURN_ON_ERROR(
      imx708_write_reg8(state, IMX708_REG_STREAMING, 0x00, "LP-11 stream-off"),
      TAG, "stream-off write failed");

  ESP_LOGI(TAG, "Sensor stream-off sent, waiting %dms for LP-11", LP11_WAIT_MS);
  vTaskDelay(pdMS_TO_TICKS(LP11_WAIT_MS));
  return ESP_OK;
}

static esp_err_t start_capture_stream(bringup_state_t *state,
                                      uint32_t first_frame_timeout_ms) {
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ESP_RETURN_ON_ERROR(sensor_stream_off_for_lp11(state), TAG,
                      "LP11 preparation failed");
  dump_csi_state("pre-streamon");

  if (ioctl(state->cap_fd, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %s", strerror(errno));
    return ESP_FAIL;
  }
  state->stream_on = true;

  uint32_t old_thrd = MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_afull_thrd;
  MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_afull_thrd = CSI_AFULL_THRESHOLD;
  ESP_LOGI(TAG, "CSI afull threshold %lu -> %d", (unsigned long)old_thrd,
           CSI_AFULL_THRESHOLD);

  int64_t t_streamon = esp_timer_get_time();
  reset_csi_isr_counters();

  ESP_LOGI(TAG, "Link poll (T+ms | buf_depth | isr_new)");
  for (int i = 0; i <= 10; i++) {
    int dt_ms = (int)((esp_timer_get_time() - t_streamon) / 1000);
    uint32_t buf_depth = MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth;
    uint32_t isr_new = csi_isr_get_new_trans_cnt;

    ESP_LOGI(TAG, "  T+%3d | dep=%lu | isr_new=%lu", dt_ms,
             (unsigned long)buf_depth, (unsigned long)isr_new);

    if (buf_depth > 0 || isr_new > 0) {
      ESP_LOGI(TAG, "  Bridge activity detected at T+%d ms", dt_ms);
      break;
    }
    if (i < 10) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  uint32_t first_bytes = 0;
  if (!try_dqbuf_with_timeout(state->cap_fd, first_frame_timeout_ms,
                              &first_bytes)) {
    int dt_ms = (int)((esp_timer_get_time() - t_streamon) / 1000);
    ESP_LOGE(TAG, "No first frame within %lums",
             (unsigned long)first_frame_timeout_ms);
    ESP_LOGI(TAG, "Link at T+%d after DQBUF: dep=%lu isr_new=%lu", dt_ms,
             (unsigned long)MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth,
             (unsigned long)csi_isr_get_new_trans_cnt);
    dump_csi_state("first-frame-timeout");
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(TAG, "First frame received: %lu bytes", (unsigned long)first_bytes);
  return ESP_OK;
}

static esp_err_t capture_smoke_frames(bringup_state_t *state,
                                      size_t frame_count) {
  for (size_t i = 0; i < frame_count; i++) {
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(state->cap_fd, VIDIOC_DQBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_DQBUF frame %lu failed: %s", (unsigned long)i,
               strerror(errno));
      dump_csi_state("dqbuf-fail");
      return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Frame %lu: bytes=%lu flags=0x%08lx index=%lu",
             (unsigned long)i, (unsigned long)buf.bytesused,
             (unsigned long)buf.flags, (unsigned long)buf.index);

    if (ioctl(state->cap_fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF frame %lu failed: %s", (unsigned long)i,
               strerror(errno));
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}

static esp_err_t heavy_pipeline_reset(bringup_state_t *state) {
  ESP_LOGW(TAG, "Heavy reset: teardown + esp_video_deinit + LDO power-cycle");

  if (state->cap_fd >= 0 && state->stream_on) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(state->cap_fd, VIDIOC_STREAMOFF, &type);
    state->stream_on = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  close_capture_pipeline(state);
  esp_err_t deinit_ret = esp_video_deinit();
  if (deinit_ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_video_deinit returned 0x%x", deinit_ret);
  }

  ESP_RETURN_ON_ERROR(power_cycle_camera_ldo(state), TAG,
                      "LDO power-cycle failed");
  ESP_RETURN_ON_ERROR(init_video_stack(state), TAG, "esp_video re-init failed");
  ESP_RETURN_ON_ERROR(open_capture_pipeline(state), TAG,
                      "pipeline re-init failed");
  ESP_RETURN_ON_ERROR(configure_isp(), TAG, "ISP re-init failed");
  return ESP_OK;
}

static esp_err_t start_streaming_with_retries(bringup_state_t *state) {
  for (int attempt = 0; attempt < MIPI_LINK_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (attempt <= MIPI_LIGHT_RETRIES) {
        ESP_LOGW(TAG,
                 "Retry %d/%d requested light reset, but light reset is "
                 "disabled for IMX708/P4",
                 attempt, MIPI_LINK_MAX_RETRIES - 1);
      } else {
        ESP_LOGW(TAG, "=== HEAVY RESET %d/%d (LDO power cycle) ===",
                 attempt - MIPI_LIGHT_RETRIES,
                 MIPI_LINK_MAX_RETRIES - MIPI_LIGHT_RETRIES - 1);
        ESP_RETURN_ON_ERROR(heavy_pipeline_reset(state), TAG,
                            "heavy reset failed");
      }
      ESP_LOGI(TAG, "Reset complete. Starting streams...");
    }

    uint32_t first_frame_timeout_ms = (attempt == 0)
                                          ? FAST_FIRST_FRAME_TIMEOUT_MS
                                          : RETRY_FIRST_FRAME_TIMEOUT_MS;
    esp_err_t ret = start_capture_stream(state, first_frame_timeout_ms);
    if (ret == ESP_OK) {
      return ESP_OK;
    }
  }

  return ESP_ERR_TIMEOUT;
}

static void cleanup(bringup_state_t *state) {
  close_capture_pipeline(state);
  esp_video_deinit();

  if (state->i2c_bus) {
    i2c_del_master_bus(state->i2c_bus);
    state->i2c_bus = NULL;
  }

  if (state->ldo_handle) {
    esp_ldo_release_channel(state->ldo_handle);
    state->ldo_handle = NULL;
  }
}

void app_main(void) {
  bringup_state_t state = {
      .cap_fd = -1,
  };

  esp_log_level_set("ISP", ESP_LOG_WARN);
  esp_log_level_set("udp-stream", ESP_LOG_INFO);
  esp_log_level_set("wifi-sta", ESP_LOG_INFO);
  esp_log_level_set("mdns-video", ESP_LOG_INFO);

  ESP_LOGI(TAG, "FireBeetle 2 + IMX708 esp_video bring-up on ESP-IDF v5.4.1");
  size_t psram_sz = esp_psram_get_size();
  ESP_LOGI(TAG, "PSRAM initialized=%s size=%u bytes (%u MB)",
           esp_psram_is_initialized() ? "yes" : "no", (unsigned)psram_sz,
           (unsigned)(psram_sz / (1024 * 1024)));
  if (psram_sz < 30 * 1024 * 1024) {
    ESP_LOGE(TAG, "PSRAM < 30MB — AI models will not fit. Aborting.");
    return;
  }

  /* Suppress ISP and focus motor debug spam so Wi-Fi/mDNS/UDP logs are visible
   */
  esp_log_level_set("ISP", ESP_LOG_WARN);
  esp_log_level_set("dw9807", ESP_LOG_WARN);

  esp_err_t ret = power_cycle_camera_ldo(&state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "power_cycle_camera_ldo failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  ret = init_i2c(&state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "init_i2c failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  ret = preemptive_camera_reset(&state);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "preemptive_camera_reset failed: 0x%x", ret);
  }

  ret = init_video_stack(&state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "init_video_stack failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  ret = open_capture_pipeline(&state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "open_capture_pipeline failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  configure_isp();

  ret = start_streaming_with_retries(&state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start_streaming_with_retries failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  ESP_LOGI(TAG, "Camera streaming started — initializing network pipeline");

  /* ── Wi-Fi: C6 creates AP, iPhone connects to it ── */
  ret = wifi_ap_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi init failed: 0x%x — streaming unavailable", ret);
    cleanup(&state);
    return;
  }

  /* ── mDNS: announce _gatavideo._udp so iPhone can discover us ── */
  ESP_LOGI(TAG, "Starting mDNS video announcement");
  ret = mdns_video_announce();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mDNS announce failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  /* ── H.264 encoder: open M2M device /dev/video11 ── */
  h264_enc_handle_t encoder = NULL;
  ESP_LOGI(TAG, "Opening H.264 encoder");
  ret = h264_enc_open(s_custom_720p_format.width, s_custom_720p_format.height,
                      &encoder);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "H.264 encoder open failed: 0x%x", ret);
    cleanup(&state);
    return;
  }

  /* ── UDP streamer: bind socket and wait for VID0 registration ── */
  udp_streamer_handle_t streamer = NULL;
  ESP_LOGI(TAG, "Starting UDP video streamer");
  ret = udp_stream_start(&streamer);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "UDP streamer start failed: 0x%x", ret);
    h264_enc_close(encoder);
    cleanup(&state);
    return;
  }

  /* Register streamer with Wi-Fi for station-disconnect notification */
  wifi_ap_set_streamer(streamer);

  /* Start AI inference task (always-on, independent of streaming) */
  ESP_LOGI(TAG, "Starting AI inference task");
  ret = ai_task_start(streamer);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "AI task failed to start (0x%x) — streaming continues", ret);
  }

  ESP_LOGI(TAG, "=== STREAMING PIPELINE READY ===");
  ESP_LOGI(TAG, "Waiting for VID0 registration from iPhone app...");

  /* ── Continuous capture loop — encode+send gated on stream state ── */
  uint32_t frame_id = 0;
  uint32_t encode_errors = 0;
  const uint32_t MAX_CONSECUTIVE_ERRORS = 10;
  bool was_idle = true;

  while (true) {
    /* DQBUF always runs — keeps camera DMA pipeline flowing */
    struct v4l2_buffer cap_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(state.cap_fd, VIDIOC_DQBUF, &cap_buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %s", strerror(errno));
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    /* Share frame with AI task (lock-free, AI reads asynchronously) */
    g_latest_yuv_frame = (const uint8_t *)state.buffers[cap_buf.index];
    g_latest_yuv_size = cap_buf.bytesused;
    g_latest_frame_id = frame_id;

    udp_stream_state_t stream_state = udp_stream_get_state(streamer);

    /* Only encode and send when client is actively receiving */
    if (stream_state == STREAM_ACTIVE) {
      if (was_idle) {
        ESP_LOGI(TAG, "Client connected — starting encode+stream");
        was_idle = false;
      }

      /* Note: force-IDR via V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME is not
         supported on ESP32-P4 HW encoder. GOP=30 gives natural IDR every
         ~1s, and the iOS decoder recreates its session on next IDR. */
      (void)udp_stream_should_force_idr(streamer); /* clear flag */

      /* Encode YUV420 → H.264 via M2M device */
      uint8_t *h264_data = NULL;
      uint32_t h264_size = 0;
      bool is_keyframe = false;

      ret = h264_enc_process(
          encoder, (const uint8_t *)state.buffers[cap_buf.index],
          cap_buf.bytesused, &h264_data, &h264_size, &is_keyframe);

      if (ret != ESP_OK) {
        encode_errors++;
        if (encode_errors >= MAX_CONSECUTIVE_ERRORS) {
          ESP_LOGE(TAG, "Too many consecutive encode errors (%lu), stopping",
                   (unsigned long)encode_errors);
          /* Re-queue before breaking */
          ioctl(state.cap_fd, VIDIOC_QBUF, &cap_buf);
          break;
        }
      } else {
        encode_errors = 0;

        /* Stream the encoded frame to the registered client */
        udp_stream_send_frame(streamer, h264_data, h264_size, frame_id,
                              s_custom_720p_format.width,
                              s_custom_720p_format.height, is_keyframe);

        if (frame_id % 300 == 0) {
          ESP_LOGI(TAG, "Streamed frame %lu: h264=%lu bytes %s",
                   (unsigned long)frame_id, (unsigned long)h264_size,
                   is_keyframe ? "[IDR]" : "");
        }

        frame_id++;
      }
    } else {
      if (!was_idle) {
        ESP_LOGI(TAG, "Client disconnected/paused — encode+stream stopped");
        was_idle = true;
      }
    }

    /* QBUF always runs — keeps camera DMA pipeline flowing */
    if (ioctl(state.cap_fd, VIDIOC_QBUF, &cap_buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF re-queue failed: %s", strerror(errno));
    }
  }

  /* Cleanup — only reached on fatal error */
  udp_stream_stop(streamer);
  h264_enc_close(encoder);
  cleanup(&state);
}
