# Third-party software

This repository vendors a modified `esp-video-components` tree.

## Espressif

The component framework, V4L2-style device layer, ISP algorithms and most sensor
support originate from Espressif Systems. Individual directories contain their
own license files and copyright notices.

## IMX708 and DW9807 community work

The IMX708 sensor and DW9807 focus-motor implementation used as the starting point
for this project comes from the public `GuilhermeRS11/esp-video-components` fork.
The local repository adds board-specific bring-up, framework integration and
end-to-end validation around that work.

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
