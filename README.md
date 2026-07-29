# ESP32-P4_IMX708

**Hardware-verified IMX708 camera capture, H.264 streaming and on-device vision on ESP32-P4.**

ESP32-P4_IMX708 is a camera and vision pipeline for the Raspberry Pi Camera Module 3 on
an ESP32-P4 system. The verified platform is a DFRobot FireBeetle 2 ESP32-P4
using its ESP32-C6 connectivity co-processor and a native iOS receiver.

The project is not an official Espressif or Raspberry Pi camera port. It combines
community IMX708 and DW9807 support with board-specific camera bring-up,
ESP32-P4 ISP configuration, hardware H.264 encoding, UDP transport, an iOS
receiver and ESP-DL inference.

## Verified result

The following path has been tested on physical hardware:

```text
Raspberry Pi Camera Module 3 (Sony IMX708)
        |
        | 2-lane MIPI CSI-2, RAW10
        v
ESP32-P4 CSI receiver and ISP
        |
        | 1280 x 720 packed YUV420
        +------------------------------+
        |                              |
        v                              v
ESP32-P4 H.264 encoder          ESP-DL inference
        |                              |
        | UDP video                    | UDP detection metadata
        v                              v
Native iOS receiver / VideoToolbox / SwiftUI overlay
```

Physical validation confirmed:

- IMX708 sensor ID `0x0708` over I2C.
- MIPI CSI activity after stream start.
- Repeated 1280 x 720 ISP frames.
- A verified frame size of 1,382,400 bytes.
- Hardware H.264 output decoded by VideoToolbox on a physical iPhone.
- End-to-end pause, resume, heartbeat and disconnect handling.
- Six ESP-DL models running alongside the video pipeline.
- Detection metadata displayed as labels and bounding boxes in the iOS app.

The camera baseline was established on 23 March 2026. End-to-end video,
application lifecycle handling and on-device inference were verified on
23-24 March 2026.

## Tested hardware

| Item | Verified configuration |
| --- | --- |
| Main board | DFRobot FireBeetle 2 ESP32-P4 |
| Main SoC | ESP32-P4, silicon revision v1.0 |
| Connectivity SoC | ESP32-C6 over ESP-Hosted SDIO |
| Camera | Raspberry Pi Camera Module 3, Sony IMX708 |
| Camera interface | 2-lane MIPI CSI-2 |
| Receiver | Physical iPhone running the included iOS app |
| Flash | 16 MB |
| PSRAM | 32 MB |

The checked-in configuration targets the ESP32-P4 v1.x range. No claim is made
for every ESP32-P4 board, later silicon revision or third-party Camera Module 3
adapter.

## Verified software baseline

| Component | Version or configuration |
| --- | --- |
| ESP-IDF | v5.4.1 |
| Target | `esp32p4` |
| Sensor input | RAW10, 1280 x 720, 30 frames/s |
| MIPI link | 2 lanes, 450 MHz link clock |
| ISP output | Packed YUV420, 1280 x 720 |
| H.264 encoder | ESP32-P4 V4L2 M2M device |
| H.264 bitrate | 800 kbit/s |
| GOP length | 30 frames |
| Video transport | Chunked UDP on port 3334 |
| Detection transport | UDP on port 3335 |
| iOS decode | VideoToolbox |
| Discovery | Bonjour service `_gatavideo._udp` |

ESP-IDF v5.4.1 is part of the verified baseline. Other framework versions have
not been validated against the same CSI, ISP and retry behavior.

## Repository contents

```text
.
├── GATA_P4_ESP_VIDEO/
│   ├── main/                         Application firmware
│   ├── components/esp-video-components/
│   │   ├── esp_cam_sensor/           Sensor and focus-motor support
│   │   ├── esp_video/                V4L2-style video devices and pipeline
│   │   ├── esp_ipa/                  ISP control algorithms
│   │   └── esp_sccb_intf/            Camera-control bus support
│   ├── partitions.csv                Flash partition layout
│   └── sdkconfig                     Verified ESP-IDF configuration
├── App_P4_Video/P4VideoViewer/       Native iOS receiver
└── docs/                             Architecture, protocol and verification notes
```

The component tree is vendored because the verified result depends on changes
that are not present in the official Espressif component release.

## Camera mode

The application uses a conservative scale-only IMX708 mode rather than the
full-resolution sensor mode.

| Parameter | Value |
| --- | --- |
| Sensor format | RAW10 |
| Output size | 1280 x 720 |
| Requested frame rate | 30 frames/s |
| Sensor input clock | 24 MHz |
| MIPI lanes | 2 |
| MIPI link clock | 450 MHz |
| Line synchronization | Disabled |
| ISP Bayer order | BGGR after sensor orientation |

The scale-only path is deliberate. Crop-mode experiments could stall the capture
pipeline on the tested ESP32-P4 v1.0 platform.

## Camera bring-up sequence

The working sequence is more involved than sensor detection alone:

1. Acquire the ESP32-P4 MIPI LDO channel at 2.5 V.
2. Remove camera power for two seconds.
3. Re-enable the LDO and allow the sensor supply to stabilize.
4. Initialize the camera-control bus on GPIO 7 and GPIO 8 at 400 kHz.
5. Write IMX708 stream-off before framework sensor probing.
6. Issue an IMX708 software reset.
7. Initialize the `esp_video` stack.
8. Select the custom 1280 x 720 RAW10 mode.
9. Configure the ISP output as packed YUV420.
10. Allocate and queue three MMAP capture buffers.
11. Force LP-11 before `VIDIOC_STREAMON`.
12. Apply the tested CSI bridge almost-full threshold.
13. Wait for the first completed frame.
14. On failure, tear down the video path, power-cycle the sensor and retry.

A full reset and resource-release path was more reliable than restarting only the
sensor stream.

## ESP32-P4 v1.0 considerations

The application avoids direct reads of selected MIPI CSI host, ISP and DW-GDMA
registers. Those diagnostics caused repeatable load faults on the tested v1.0
path and could obscure the real camera state. Runtime diagnostics therefore use
safe CSI bridge fields and interrupt counters.

The verified configuration uses the ISP in the normal non-bypass path. Earlier
raw-CSI experiments using ISP bypass did not produce completed DMA transfers on
the tested hardware.

## Packed YUV420 layout

The ISP output used by this project is not I420, NV12 or NV21. It is a
line-interleaved packed layout referred to here as `O_UYY_E_VYY`:

```text
Even row: U Y Y | U Y Y | U Y Y | ...
Odd row:  V Y Y | V Y Y | V Y Y | ...
```

Each three-byte group carries two luma samples and one chroma sample. Adjacent
even and odd rows provide the U/V pair required for 4:2:0 sampling.

For width `W` and height `H`:

```text
row_stride = W * 3 / 2
frame_size = row_stride * H
```

At 1280 x 720:

```text
1280 * 3 / 2 * 720 = 1,382,400 bytes
```

The inference path decodes this layout directly, performs nearest-neighbour
decimation in the same pass and applies full-range BT.601 coefficients. Output
bytes are written in true RGB order.

The converter retains the historical file name `yuv_to_rgb_hw.c` for interface
compatibility, but the current implementation is software-based and does not use
PPA for this conversion.

## H.264 path

The encoder wrapper uses the ESP32-P4 V4L2 memory-to-memory device:

- OUTPUT queue: YUV420 input through `V4L2_MEMORY_USERPTR`.
- CAPTURE queue: H.264 output through `V4L2_MEMORY_MMAP`.
- GOP length: 30 frames.
- Target bitrate: 800 kbit/s.
- Quantizer range: 25 to 40.

The hardware encoder does not support the force-keyframe control used by some
Linux V4L2 encoders. A reconnecting client therefore waits for the next natural
IDR frame.

## Network architecture

The ESP32-P4 has no native Wi-Fi radio. The FireBeetle board uses its ESP32-C6
co-processor through ESP-Hosted over four-bit SDIO at 40 MHz.

The verified configuration uses access-point mode:

- SSID: `GATA_P4_VIDEO`
- Default password: `gatap4cam`
- Channel: 6
- AP address: `192.168.4.1`
- DHCP server: enabled
- Wi-Fi power saving: disabled

These are development defaults and should be changed before deployment outside a
controlled test environment.

Station mode was not used in the verified configuration because DHCP broadcast
traffic did not pass reliably through the tested hosted-SDIO path. Access-point
mode keeps address assignment on the board and was stable during physical tests.

## Video and control protocol

Video and control messages share UDP port 3334. All control messages are four
ASCII bytes:

| Message | Direction | Meaning |
| --- | --- | --- |
| `VID0` | App to firmware | Register the client and begin streaming |
| `VACK` | Firmware to app | Confirm registration |
| `BEAT` | App to firmware | Keep the active session alive |
| `PAWS` | App to firmware | Pause while the app is in the background |
| `GONE` | App to firmware | Close the session |

The firmware state machine has `IDLE`, `ACTIVE` and `PAUSED` states. A client is
returned to `IDLE` after three seconds without a heartbeat or after Wi-Fi
disconnection.

Each encoded frame is split into payloads of at most 1400 bytes. Every datagram
starts with a 28-byte little-endian header containing frame identity, dimensions,
timestamp, total frame length, chunk index, chunk count and keyframe state.
Incomplete frames are discarded rather than sent to the decoder.

Detailed field definitions are in [docs/WIRE_PROTOCOL.md](docs/WIRE_PROTOCOL.md).

## Native iOS receiver

The included SwiftUI application provides:

- Bonjour discovery of `_gatavideo._udp`.
- UDP registration and heartbeat handling.
- Out-of-order H.264 chunk reassembly.
- Annex-B NAL-unit parsing.
- SPS/PPS extraction.
- Annex-B to AVCC conversion.
- VideoToolbox hardware decoding.
- SwiftUI video presentation.
- A second UDP receiver for object-detection metadata.
- Bounding-box and label rendering.
- Pause and resume behavior tied to the iOS scene lifecycle.

The network parsers use unaligned-safe little-endian reads because packed headers
must not be interpreted through alignment-dependent loads.

## On-device inference

The ESP32-P4 runs six ESP-DL models sequentially:

- face detection
- pedestrian detection
- cat detection
- dog detection
- hand detection
- YOLO11n COCO detection

The five smaller detectors form the fast sweep. YOLO runs periodically because
its inference time is substantially longer. Results are transmitted on UDP port
3335 as compact binary records.

The verified configuration loaded all six models and retained approximately
10.9 MB of free PSRAM. Inference and video capture ran concurrently on separate
cores.

The current capture-to-inference handoff shares the newest capture-buffer pointer
without copying a complete frame. This worked in the verified timing
configuration, but it is not a strict ownership mechanism. A production design
should add explicit ownership or a snapshot buffer before increasing load or
changing task priorities.

## Autofocus

Camera Module 3 focus is driven through the DW9807 voice-coil motor controller.
The vendored component includes:

- I2C device detection
- direct 10-bit position control
- configurable initial position
- ISP autofocus windows
- coarse and fine scan parameters

The included IPA configuration contains exposure, gain, white-balance,
color-correction and autofocus parameters. These values are project tuning data,
not a universal calibration for every Camera Module 3 unit or lighting condition.

## Verification evidence

A successful camera session is defined by all of the following:

1. IMX708 sensor ID `0x0708` is read.
2. The custom 1280 x 720 mode is selected.
3. CSI activity begins after stream start.
4. A first frame of exactly 1,382,400 bytes is dequeued.
5. Subsequent frames continue to dequeue and requeue.
6. The H.264 device produces encoded output.
7. The receiver creates a VideoToolbox session from SPS/PPS.
8. Decoded video is displayed on physical hardware.

Additional test records are in [docs/VERIFICATION.md](docs/VERIFICATION.md).

## Known limitations

- Only the listed FireBeetle 2 ESP32-P4 v1.0 configuration is verified.
- The verified framework baseline includes project-specific changes to ESP-IDF
  v5.4.1 and the vendored video components.
- Full-resolution 4608 x 2592 real-time ISP processing is not the target.
- UDP does not retransmit missing chunks.
- The H.264 encoder cannot be forced to emit an immediate IDR frame.
- Camera tuning is not a production calibration.
- The inference handoff does not provide strict capture-buffer ownership.
- Default Wi-Fi credentials are embedded in the firmware configuration header.
- There is no automated hardware-in-the-loop test suite.

## Technical documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Wire protocol](docs/WIRE_PROTOCOL.md)
- [Hardware verification record](docs/VERIFICATION.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Third-party software](docs/THIRD_PARTY.md)

## Third-party components and attribution

The IMX708 and DW9807 implementation in the vendored component tree is based on
the public `GuilhermeRS11/esp-video-components` fork and Espressif's
`esp-video-components` framework. Public Raspberry Pi and Linux camera material
was used as a technical reference for IMX708 modes and controls.

Third-party files retain their original copyright notices and license terms. See
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) and the license files inside the
vendored component directories.

## Licensing

This is a [mixed-license source distribution](LICENSE), not an Apache-only
repository.

- Project-authored application source, the iOS receiver and documentation are
  Apache-2.0 unless a more specific notice applies.
- The active `imx708_regs.h` and `imx708_settings.h` files contain register
  material derived in substantial part from the Raspberry Pi Linux IMX708
  driver and are GPL-2.0-only.
- Vendored Espressif components, Linux UAPI headers and reference files retain
  their own licenses.

No prebuilt firmware binary is distributed. A firmware image built from the
complete source includes GPL-2.0-only material and is not covered by Apache-2.0
alone. See the [license map](LICENSES/README.md), the detailed
[licensing notes](docs/LICENSING.md) and [third-party attribution](docs/THIRD_PARTY.md).
