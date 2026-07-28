# Third-party software and license map

This repository vendors a modified `esp-video-components` tree. The top-level
Apache-2.0 license applies only where a file or directory does not state another
license. File-level SPDX identifiers and the nearest directory license take
precedence.

## Espressif components

The upstream project uses different licenses per component:

| Path | License |
| --- | --- |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/` | Apache-2.0, except the reference files listed below |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_sccb_intf/` | Apache-2.0 |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_video/` | ESPRESSIF MIT License, except files with their own SPDX header |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_ipa/` | ESPRESSIF MIT License |

The authoritative component license texts remain in each component directory.
The ESPRESSIF MIT License permits use on Espressif Systems products; this project
targets the ESP32-P4.

## IMX708 and DW9807 community work

The IMX708 sensor and DW9807 focus-motor implementation used as the starting
point comes from the public `GuilhermeRS11/esp-video-components` fork. The local
repository adds board-specific bring-up, framework integration, ISP tuning and
end-to-end hardware validation around that work. Existing notices and license
headers are retained.

## GPL-2.0-only reference sources

The following files are redistributed as reference material under GPL-2.0-only:

- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/reference/imx708_ref_raspberry_pi.c`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/motors/dw9714/references_dw9807/dw9807-vcm.c`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/referencia/imx708_RPi_ref.c`

They are not listed in the ESP-IDF CMake component sources or component
manifests and are not part of the firmware build. A copy of GPL-2.0 is provided
at [`LICENSES/GPL-2.0-only.txt`](../LICENSES/GPL-2.0-only.txt).

## BSD reference material

`GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/reference/cam_helper_imx708.cpp`
is licensed under BSD-2-Clause. The license text is provided at
[`LICENSES/BSD-2-Clause.txt`](../LICENSES/BSD-2-Clause.txt).

## Linux UAPI headers

The V4L2 headers under
`GATA_P4_ESP_VIDEO/components/esp-video-components/esp_video/include/linux/`
retain their Linux SPDX expressions and embedded notices. In particular,
`v4l2-common.h`, `v4l2-controls.h` and `videodev2.h` are distributed under
`((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause)`. Their file headers are
authoritative.

## Raspberry Pi references

Public Raspberry Pi and Linux camera-driver material was used as a technical
reference for IMX708 modes and controls. Raspberry Pi Camera Module 3, Raspberry
Pi and related names are trademarks of their respective owners.

## Apple

The iOS receiver uses public Apple frameworks including Network, VideoToolbox,
CoreImage, AVFoundation and SwiftUI. Apple and iPhone are trademarks of Apple Inc.

## DFRobot

The verified board is the DFRobot FireBeetle 2 ESP32-P4. DFRobot and FireBeetle
are trademarks of their respective owner.

No affiliation or official endorsement is implied.
