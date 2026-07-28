# Architecture

## Data paths

The firmware maintains three related data paths.

### Capture and video

```text
IMX708 RAW10 -> MIPI CSI-2 -> ESP32-P4 ISP -> packed YUV420
             -> V4L2 H.264 M2M encoder -> UDP 3334 -> iPhone VideoToolbox
```

Capture uses three MMAP buffers. The application continuously dequeues and
requeues them so the CSI DMA pipeline remains active even when no iPhone client
is registered. H.264 encoding and transmission are gated by the UDP session
state.

### Inference

```text
latest packed YUV420 frame -> direct packed-layout RGB conversion + decimation
                           -> ESP-DL models -> UDP 3335 -> SwiftUI overlay
```

The conversion path reads the ISP's line-interleaved U/V layout directly. It does
not reinterpret the frame as a standard planar or semi-planar YUV format.

### Control

```text
iPhone VID0/BEAT/PAWS/GONE -> UDP 3334 -> IDLE/ACTIVE/PAUSED state machine
firmware VACK               -> UDP 3334 -> iPhone
```

A three-second heartbeat timeout returns the firmware to IDLE. Wi-Fi station
disconnection also clears the active client.

## Task placement

- Camera capture and streaming remain on the primary application path.
- The inference task is pinned to the second core at lower priority.
- The UDP control listener is a separate FreeRTOS task.
- The iOS application performs network receive, frame assembly and hardware
  decode asynchronously.

## Memory

At 1280 x 720, one packed YUV420 frame is 1,382,400 bytes. Three capture buffers
therefore occupy roughly 4.15 MB before encoder, network and inference buffers.
The ESP-DL models and RGB working buffers are placed in PSRAM.

The H.264 encoder uses one USERPTR input buffer reference and one MMAP output
buffer. The inference path allocates independent 224 x 224 and 320 x 320 RGB888
buffers but currently reads the camera buffer without taking ownership of it.

## Recovery model

A failed first-frame wait is treated as a link-state failure rather than a normal
empty queue. Recovery performs:

1. stream-off
2. buffer unmap and device close
3. `esp_video_deinit()`
4. camera LDO power cycle
5. video-stack reinitialization
6. format and ISP reconfiguration
7. a new first-frame test

This sequence was more reliable than trying to restart only the sensor stream.
