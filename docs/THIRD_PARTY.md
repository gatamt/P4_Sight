# Third-party software, provenance and licenses

This repository vendors a modified `esp-video-components` tree and contains
reference material from several upstream projects. File-level SPDX identifiers
and the nearest directory license take precedence over project defaults.

## Espressif components

| Path | License |
| --- | --- |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/` | Apache-2.0, except the active and reference exceptions below |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_sccb_intf/` | Apache-2.0 |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_video/` | ESPRESSIF MIT License, except files with their own SPDX notice |
| `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_ipa/` | ESPRESSIF MIT License |

The authoritative license texts remain in the component directories. The
ESPRESSIF MIT License is product-scoped and permits use on Espressif Systems
products; this project targets ESP32-P4.

## IMX708 and DW9807 community implementation

The IMX708 sensor and DW9807 focus-motor implementation used as the starting
point comes from the public `GuilhermeRS11/esp-video-components` fork. The local
project adds board-specific power sequencing, camera bring-up, framework
integration, ISP configuration and end-to-end hardware validation.

The active IMX708 implementation code in `imx708.c` retains its Apache-2.0
notice. A source comparison found no substantial function-level or long-comment
copying from the retained GPL Linux references. The register layer has different
provenance and is licensed separately as described below.

## Active IMX708 register files

The following files contain register definitions or register sequences derived
in substantial part from the Raspberry Pi Linux IMX708 driver:

- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/private_include/imx708_regs.h`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/private_include/imx708_settings.h`

They are distributed under GPL-2.0-only and carry explicit SPDX notices. This
corrects the earlier permissive-only headers and avoids presenting the register
material as Apache-2.0-only.

The register values are hardware-facing configuration data required to operate
the sensor. Their functional nature does not justify removing the upstream
license notice while their provenance remains derived from the Linux source.

## GPL-2.0-only reference sources

The following files are retained as reference material under GPL-2.0-only:

- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/reference/imx708_ref_raspberry_pi.c`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/motors/dw9714/references_dw9807/dw9807-vcm.c`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/referencia/imx708_RPi_ref.c`

They are not listed in the ESP-IDF component build sources. The complete license
text is in [`../LICENSES/GPL-2.0-only.txt`](../LICENSES/GPL-2.0-only.txt).

## BSD reference material

`GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/reference/cam_helper_imx708.cpp`
is licensed under BSD-2-Clause. The complete text is in
[`../LICENSES/BSD-2-Clause.txt`](../LICENSES/BSD-2-Clause.txt).

## Linux UAPI headers

The V4L2 headers below
`GATA_P4_ESP_VIDEO/components/esp-video-components/esp_video/include/linux/`
retain their embedded SPDX expressions and notices. In particular,
`v4l2-common.h`, `v4l2-controls.h` and `videodev2.h` state
`((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause)`. Their file headers are
authoritative.

## Raspberry Pi references and trademarks

Public Raspberry Pi and Linux camera-driver material was used for IMX708 modes,
controls and register behavior. Raspberry Pi Camera Module 3, Raspberry Pi and
related names are trademarks of their respective owners. No affiliation or
endorsement is implied.

## Apple and DFRobot

The iOS receiver uses public Apple frameworks including Network, VideoToolbox,
CoreImage, AVFoundation and SwiftUI. The verified board is the DFRobot
FireBeetle 2 ESP32-P4. Apple, iPhone, DFRobot and FireBeetle are trademarks of
their respective owners. No affiliation or endorsement is implied.

## Combined firmware

No prebuilt firmware binary is distributed here. A firmware image built with the
active IMX708 register files contains GPL-2.0-only material and is not covered by
Apache-2.0 alone. This repository does not assert that every linked component can
be redistributed together under one permissive license.
