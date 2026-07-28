/**
 * @file ai_task.cpp
 * @brief Always-on AI inference task — 6 models running sequentially.
 *
 * Models (sequential per frame):
 *   Fast sweep (~4x/sec):
 *     1. HumanFaceDetect   — 19.6ms, faces
 *     2. PedestrianDetect  — 60.5ms, persons
 *     3. CatDetect         — 54ms,   cats
 *     4. DogDetect         — 54ms,   dogs
 *     5. HandDetect        — 50.5ms, hands
 *   Periodic YOLO sweep (~1x/sec):
 *     6. YOLO11n-320       — 611ms,  80 COCO classes
 *
 * Runs on core 1, priority 3. Camera capture stays on core 0.
 */

#include "ai_task.h"
#include "bbox_sender.h"
#include "yuv_to_rgb_hw.h"

#include <cstring>
#include <list>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* ESP-DL model headers */
#include "cat_detect.hpp"
#include "coco_detect.hpp"
#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "dog_detect.hpp"
#include "hand_detect.hpp"
#include "human_face_detect.hpp"
#include "pedestrian_detect.hpp"

#define MODELS_FACE_ONLY 0

static const char *TAG = "ai-task";

#define AI_TASK_STACK_SIZE (32 * 1024)
#define AI_TASK_PRIORITY 3
#define AI_TASK_CORE 1
#define FAST_MODEL_INPUT_W 224
#define FAST_MODEL_INPUT_H 224
#define YOLO_INPUT_W 320
#define YOLO_INPUT_H 320
#define SRC_FRAME_W 1280
#define SRC_FRAME_H 720
#define YOLO_SWEEP_INTERVAL 3 /* Run YOLO every N fast sweeps */

/* Frame sharing globals — written by capture loop in camera_bringup.c */
extern "C" {
extern volatile const uint8_t *g_latest_yuv_frame;
extern volatile uint32_t g_latest_yuv_size;
extern volatile uint32_t g_latest_frame_id;
}

/* Shared detection results */
static ai_result_buffer_t s_results;

/* Streamer handle for bbox sending */
static udp_streamer_handle_t s_streamer = NULL;

/* ── COCO 80 class names ──────────────────────────────────────────── */

static const char *COCO_CLASSES[80] = {
    "person",        "bicycle",      "car",
    "motorcycle",    "airplane",     "bus",
    "train",         "truck",        "boat",
    "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench",        "bird",
    "cat",           "dog",          "horse",
    "sheep",         "cow",          "elephant",
    "bear",          "zebra",        "giraffe",
    "backpack",      "umbrella",     "handbag",
    "tie",           "suitcase",     "frisbee",
    "skis",          "snowboard",    "sports ball",
    "kite",          "baseball bat", "baseball glove",
    "skateboard",    "surfboard",    "tennis racket",
    "bottle",        "wine glass",   "cup",
    "fork",          "knife",        "spoon",
    "bowl",          "banana",       "apple",
    "sandwich",      "orange",       "broccoli",
    "carrot",        "hot dog",      "pizza",
    "donut",         "cake",         "chair",
    "couch",         "potted plant", "bed",
    "dining table",  "toilet",       "tv",
    "laptop",        "mouse",        "remote",
    "keyboard",      "cell phone",   "microwave",
    "oven",          "toaster",      "sink",
    "refrigerator",  "book",         "clock",
    "vase",          "scissors",     "teddy bear",
    "hair drier",    "toothbrush",
};

/**
 * @brief Add detections from an ESP-DL model to the result buffer.
 *
 * Scales coordinates from model input resolution to source frame (1280x720).
 */
static void collect_detections(const std::list<dl::detect::result_t> &results,
                               const char *label, uint8_t class_id_base,
                               uint32_t model_w, uint32_t model_h,
                               ai_detection_t *out, uint8_t *count) {
  for (const auto &r : results) {
    if (*count >= AI_MAX_DETECTIONS) {
      break;
    }

    ai_detection_t *d = &out[*count];
    d->class_id = class_id_base + (uint8_t)r.category;
    d->confidence = (uint8_t)(r.score * 100.0f);

    /* Scale from model resolution to source frame */
    d->x1 = (uint16_t)((r.box[0] * SRC_FRAME_W) / (int)model_w);
    d->y1 = (uint16_t)((r.box[1] * SRC_FRAME_H) / (int)model_h);
    d->x2 = (uint16_t)((r.box[2] * SRC_FRAME_W) / (int)model_w);
    d->y2 = (uint16_t)((r.box[3] * SRC_FRAME_H) / (int)model_h);

    /* Clamp to frame bounds */
    if (d->x2 > SRC_FRAME_W)
      d->x2 = SRC_FRAME_W;
    if (d->y2 > SRC_FRAME_H)
      d->y2 = SRC_FRAME_H;

    /* Set label */
    if (label != NULL) {
      strncpy(d->label, label, AI_LABEL_MAX_LEN - 1);
      d->label[AI_LABEL_MAX_LEN - 1] = '\0';
    } else {
      /* COCO class lookup */
      int cls = r.category;
      if (cls >= 0 && cls < 80) {
        strncpy(d->label, COCO_CLASSES[cls], AI_LABEL_MAX_LEN - 1);
        d->label[AI_LABEL_MAX_LEN - 1] = '\0';
      } else {
        snprintf(d->label, AI_LABEL_MAX_LEN, "cls_%d", cls);
      }
    }

    (*count)++;
  }
}

/**
 * @brief AI inference task — runs all 6 models sequentially.
 */
static void ai_inference_task(void *arg) {
  ESP_LOGI(TAG, "AI inference task started on core %d", xPortGetCoreID());

  /* Initialize the packed YUV420-to-RGB conversion path. */
  esp_err_t hw_ret = yuv_to_rgb_hw_init();
  if (hw_ret != ESP_OK) {
    ESP_LOGE(TAG, "PPA init failed: %s — AI task aborted",
             esp_err_to_name(hw_ret));
    vTaskDelete(NULL);
    return;
  }

  /* Allocate cache-aligned RGB output buffers in PSRAM. */
  uint8_t *rgb_fast = NULL;
  uint32_t rgb_fast_size = 0;
  uint8_t *rgb_yolo = NULL;
  uint32_t rgb_yolo_size = 0;

  if (yuv_to_rgb_hw_alloc_buf(FAST_MODEL_INPUT_W, FAST_MODEL_INPUT_H, &rgb_fast,
                              &rgb_fast_size) != ESP_OK ||
      yuv_to_rgb_hw_alloc_buf(YOLO_INPUT_W, YOLO_INPUT_H, &rgb_yolo,
                              &rgb_yolo_size) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to allocate aligned RGB buffers in PSRAM");
    heap_caps_free(rgb_fast);
    heap_caps_free(rgb_yolo);
    yuv_to_rgb_hw_deinit();
    vTaskDelete(NULL);
    return;
  }

  /* Initialize bbox sender */
  bbox_sender_init();

  /* Load models */
  ESP_LOGI(TAG, "Loading AI models...");
  int64_t t0 = esp_timer_get_time();

  HumanFaceDetect *face_det = new HumanFaceDetect();
  ESP_LOGI(TAG, "  HumanFaceDetect loaded");

#if !MODELS_FACE_ONLY
  PedestrianDetect *ped_det = new PedestrianDetect();
  ESP_LOGI(TAG, "  PedestrianDetect loaded");

  CatDetect *cat_det = new CatDetect();
  ESP_LOGI(TAG, "  CatDetect loaded");

  DogDetect *dog_det = new DogDetect();
  ESP_LOGI(TAG, "  DogDetect loaded");

  HandDetect *hand_det = new HandDetect();
  ESP_LOGI(TAG, "  HandDetect loaded");

  COCODetect *yolo_det = new COCODetect();
  ESP_LOGI(TAG, "  COCODetect (YOLO11n-320) loaded");

  int64_t load_ms = (esp_timer_get_time() - t0) / 1000;
  ESP_LOGI(TAG, "All 6 models loaded in %lld ms", load_ms);
#else
  int64_t load_ms = (esp_timer_get_time() - t0) / 1000;
  ESP_LOGI(TAG, "Face-only model loaded in %lld ms (other models disabled)",
           load_ms);
#endif

  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "Free PSRAM after model load: %u KB",
           (unsigned)(free_psram / 1024));

  uint32_t sweep_count = 0;
  uint32_t last_frame_id = UINT32_MAX;

  /* ── Main inference loop ─────────────────────────────────────── */
  while (true) {
    /* Read latest frame pointer */
    const uint8_t *frame = (const uint8_t *)g_latest_yuv_frame;
    uint32_t frame_size = g_latest_yuv_size;
    uint32_t frame_id = g_latest_frame_id;

    if (frame == NULL || frame_size == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    /* Skip if same frame as last time */
    if (frame_id == last_frame_id) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    last_frame_id = frame_id;

    /* Cache sync: flush L2 cache so CPU reads fresh DMA data from PSRAM */
    esp_cache_msync((void *)frame, frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* DIAGNOSTIC: hex dump first 16 bytes after Y plane to determine chroma
     * layout. I420 planar:  U0 U1 U2 U3 U4 U5 ... (smooth gradient, all same
     * channel) NV12 semi:    U0 V0 U1 V1 U2 V2 ... (alternating pairs) NV21
     * semi:    V0 U0 V1 U1 V2 U2 ... (alternating pairs, reversed) */
    if (sweep_count < 2) {
      const uint8_t *chroma = frame + (SRC_FRAME_W * SRC_FRAME_H);
      ESP_LOGI(TAG, "CHROMA hex dump (first 16 bytes after Y plane):");
      ESP_LOGI(TAG,
               "  [%02x %02x %02x %02x %02x %02x %02x %02x  "
               "%02x %02x %02x %02x %02x %02x %02x %02x]",
               chroma[0], chroma[1], chroma[2], chroma[3], chroma[4], chroma[5],
               chroma[6], chroma[7], chroma[8], chroma[9], chroma[10],
               chroma[11], chroma[12], chroma[13], chroma[14], chroma[15]);
      /* Also dump bytes at offset 640 (start of second chroma row for NV12)
       * vs offset 320 (start of second U row for I420, stride=640) */
      ESP_LOGI(
          TAG,
          "  row2 @640: [%02x %02x %02x %02x]  @320: [%02x %02x %02x %02x]",
          chroma[640], chroma[641], chroma[642], chroma[643], chroma[320],
          chroma[321], chroma[322], chroma[323]);
    }

    /* Decode packed YUV420 directly to 224 x 224 RGB888 with decimation. */
    esp_err_t conv_ret =
        yuv_to_rgb_hw_convert(frame, SRC_FRAME_W, SRC_FRAME_H, rgb_fast,
                              FAST_MODEL_INPUT_W, FAST_MODEL_INPUT_H);
    if (conv_ret != ESP_OK) {
      ESP_LOGW(TAG, "HW CSC failed (%s), skipping frame",
               esp_err_to_name(conv_ret));
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    /* Prepare image struct for fast models */
    dl::image::img_t img_fast = {
        .data = rgb_fast,
        .width = FAST_MODEL_INPUT_W,
        .height = FAST_MODEL_INPUT_H,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    /* Accumulate detections from all models */
    ai_detection_t temp_dets[AI_MAX_DETECTIONS];
    uint8_t num_dets = 0;
    int64_t sweep_start = esp_timer_get_time();

    /* 1. Face detection (~19.6ms) */
    auto &face_res = face_det->run(img_fast);
    collect_detections(face_res, "face", 128, FAST_MODEL_INPUT_W,
                       FAST_MODEL_INPUT_H, temp_dets, &num_dets);

#if !MODELS_FACE_ONLY
    /* 2. Pedestrian detection (~60.5ms) */
    auto &ped_res = ped_det->run(img_fast);
    collect_detections(ped_res, "person", 129, FAST_MODEL_INPUT_W,
                       FAST_MODEL_INPUT_H, temp_dets, &num_dets);

    /* 3. Cat detection (~54ms) */
    auto &cat_res = cat_det->run(img_fast);
    collect_detections(cat_res, "cat", 130, FAST_MODEL_INPUT_W,
                       FAST_MODEL_INPUT_H, temp_dets, &num_dets);

    /* 4. Dog detection (~54ms) */
    auto &dog_res = dog_det->run(img_fast);
    collect_detections(dog_res, "dog", 131, FAST_MODEL_INPUT_W,
                       FAST_MODEL_INPUT_H, temp_dets, &num_dets);

    /* 5. Hand detection (~50.5ms) */
    auto &hand_res = hand_det->run(img_fast);
    collect_detections(hand_res, "hand", 132, FAST_MODEL_INPUT_W,
                       FAST_MODEL_INPUT_H, temp_dets, &num_dets);

    /* 6. YOLO11n-320 every Nth sweep (~611ms) */
    if (sweep_count % YOLO_SWEEP_INTERVAL == 0) {
      esp_err_t yolo_ret =
          yuv_to_rgb_hw_convert(frame, SRC_FRAME_W, SRC_FRAME_H, rgb_yolo,
                                YOLO_INPUT_W, YOLO_INPUT_H);
      if (yolo_ret != ESP_OK) {
        ESP_LOGW(TAG, "HW CSC for YOLO failed, skipping YOLO this sweep");
        goto skip_yolo;
      }

      dl::image::img_t img_yolo = {
          .data = rgb_yolo,
          .width = YOLO_INPUT_W,
          .height = YOLO_INPUT_H,
          .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
      };

      auto &yolo_res = yolo_det->run(img_yolo);
      collect_detections(yolo_res, NULL, 0, YOLO_INPUT_W, YOLO_INPUT_H,
                         temp_dets, &num_dets);
    }
  skip_yolo:
#endif

    int64_t sweep_ms = (esp_timer_get_time() - sweep_start) / 1000;

    /* Write results atomically (seqlock) */
    uint32_t seq = s_results.sequence + 1;
    s_results.sequence = seq;
    __asm__ volatile("" ::: "memory"); /* compiler barrier */
    s_results.frame_id = frame_id;
    s_results.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_results.num_detections = num_dets;
    memcpy((void *)s_results.detections, temp_dets,
           num_dets * sizeof(ai_detection_t));
    __asm__ volatile("" ::: "memory");
    s_results.sequence = seq + 1;

    /* Log periodically */
    if (sweep_count % 10 == 0) {
      ESP_LOGI(TAG, "Sweep #%lu: %u detections in %lld ms (frame %lu)",
               (unsigned long)sweep_count, num_dets, sweep_ms,
               (unsigned long)frame_id);
    }

    /* Send bbox to iPhone if connected */
    struct sockaddr_in client_addr;
    if (udp_stream_get_client_addr(s_streamer, &client_addr)) {
      bbox_sender_send(&s_results, &client_addr);
    }

    sweep_count++;
  }
}

/* ── Public C API ─────────────────────────────────────────────────── */

extern "C" esp_err_t ai_task_start(udp_streamer_handle_t streamer) {
  s_streamer = streamer;
  memset(&s_results, 0, sizeof(s_results));

  BaseType_t ret =
      xTaskCreatePinnedToCore(ai_inference_task, "ai_infer", AI_TASK_STACK_SIZE,
                              NULL, AI_TASK_PRIORITY, NULL, AI_TASK_CORE);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create AI task");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "AI inference task launched (core=%d, prio=%d)", AI_TASK_CORE,
           AI_TASK_PRIORITY);
  return ESP_OK;
}

extern "C" const ai_result_buffer_t *ai_task_get_results(void) {
  return &s_results;
}
