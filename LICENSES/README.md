# License map

P4_Sight is a mixed-license source distribution. It is not licensed as a whole
under Apache-2.0 or any other single license.

## Precedence

1. A file-level SPDX identifier or license notice controls that file.
2. Otherwise, the nearest license file in the directory tree controls.
3. Otherwise, project-authored material is licensed under Apache-2.0.

## Project-authored material

Unless a more specific notice applies, the following project-authored material
is licensed under Apache-2.0:

- `GATA_P4_ESP_VIDEO/main/`
- `App_P4_Video/P4VideoViewer/`
- root project configuration and project-authored documentation

The complete license text is in [`Apache-2.0.txt`](Apache-2.0.txt).

The Apache-2.0 grant applies to the source files themselves. It does not turn a
combined firmware image containing GPL-2.0-only material into an Apache-only
work.

## Active IMX708 GPL material

The following active files contain register definitions or sequences derived in
substantial part from the Raspberry Pi Linux IMX708 driver and are distributed
under GPL-2.0-only:

- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/private_include/imx708_regs.h`
- `GATA_P4_ESP_VIDEO/components/esp-video-components/esp_cam_sensor/sensors/imx708/private_include/imx708_settings.h`

The complete license text is in
[`GPL-2.0-only.txt`](GPL-2.0-only.txt).

## Vendored components and references

| Path or material | License |
| --- | --- |
| `.../esp_cam_sensor/` | Apache-2.0, except files explicitly listed in this map or carrying another notice |
| `.../esp_sccb_intf/` | Apache-2.0 |
| `.../esp_video/` | ESPRESSIF MIT License, except files with their own SPDX notice |
| `.../esp_ipa/` | ESPRESSIF MIT License |
| Three retained Raspberry Pi/Linux driver reference files | GPL-2.0-only |
| `.../reference/cam_helper_imx708.cpp` | BSD-2-Clause |
| Linux V4L2 UAPI headers | Their embedded Linux SPDX expressions, including dual-license terms where stated |

The adjacent component license files and file-level notices are authoritative.
See [`../docs/THIRD_PARTY.md`](../docs/THIRD_PARTY.md) for exact paths and
provenance.

## Binary distribution

No prebuilt firmware binary is published in this repository. A firmware build
that incorporates the active GPL-2.0-only IMX708 files is not licensed under
Apache-2.0 alone. No claim is made here that all linked components can be
redistributed together under one license.
