/**
 * @file ai_detect_types.h
 * @brief Shared types for AI detection results and bounding box wire protocol.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MAX_DETECTIONS 20
#define AI_LABEL_MAX_LEN 16

/* ── Detection result (in-memory) ─────────────────────────────────── */

/** One detected object. Coordinates in source frame space (1280x720). */
typedef struct {
  uint8_t class_id;             /**< Class identifier (model-specific) */
  uint8_t confidence;           /**< 0-100 percentage */
  uint16_t x1, y1;              /**< Top-left corner */
  uint16_t x2, y2;              /**< Bottom-right corner */
  char label[AI_LABEL_MAX_LEN]; /**< Human-readable name, null-terminated */
} ai_detection_t;

/**
 * @brief Shared detection result buffer (seqlock pattern).
 *
 * Writer (AI task) increments sequence AFTER writing all fields.
 * Reader samples sequence before and after — if equal and different
 * from last read, data is consistent.
 */
typedef struct {
  volatile uint32_t sequence;      /**< Increments on every write */
  volatile uint32_t frame_id;      /**< Camera frame these came from */
  volatile uint32_t timestamp_ms;  /**< esp_timer / 1000 at detection */
  volatile uint8_t num_detections; /**< Number of valid entries (0-20) */
  ai_detection_t detections[AI_MAX_DETECTIONS];
} ai_result_buffer_t;

/* ── Bounding box UDP wire format (port 3335) ────────────────────── */

/** Bbox packet header — little-endian. */
typedef struct __attribute__((packed)) {
  uint8_t magic[4];       /**< "BBOX" */
  uint32_t frame_id;      /**< Camera frame ID */
  uint32_t timestamp_ms;  /**< Detection timestamp */
  uint8_t num_detections; /**< 0-20 */
} bbox_packet_header_t;

/** One detection in the wire format. */
typedef struct __attribute__((packed)) {
  uint8_t class_id;
  uint8_t confidence;      /**< 0-100 */
  uint16_t x1, y1, x2, y2; /**< In source frame coords (1280x720) */
  uint8_t label_len;       /**< Length of label (max 15) */
  char label[15];          /**< NOT null-terminated in packet */
} bbox_detection_wire_t;

#ifdef __cplusplus
static_assert(sizeof(bbox_packet_header_t) == 13,
              "bbox header must be 13 bytes");
static_assert(sizeof(bbox_detection_wire_t) == 26,
              "bbox detection must be 26 bytes");
#else
_Static_assert(sizeof(bbox_packet_header_t) == 13,
               "bbox header must be 13 bytes");
_Static_assert(sizeof(bbox_detection_wire_t) == 26,
               "bbox detection must be 26 bytes");
#endif

#ifdef __cplusplus
}
#endif
