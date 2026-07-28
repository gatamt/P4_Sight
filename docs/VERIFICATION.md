# Hardware verification record

## Camera baseline

Date: 23 March 2026

Test platform:

- DFRobot FireBeetle 2 ESP32-P4
- ESP32-P4 silicon revision v1.0
- Raspberry Pi Camera Module 3 with Sony IMX708
- ESP-IDF v5.4.1 release tree

Observed evidence:

- sensor identifier `0x0708`
- no unintended external XCLK configuration during sensor detection
- successful `VIDIOC_STREAMON`
- CSI bridge activity approximately 38 ms after start
- first frame size 1,382,400 bytes
- repeated subsequent frames
- completed camera smoke test

## End-to-end video

Date: 23 March 2026

Observed path:

```text
IMX708 -> ISP -> YUV420 -> hardware H.264 -> UDP -> Wi-Fi AP
       -> iPhone 13 Pro -> VideoToolbox -> displayed frame
```

The iOS application parsed SPS/PPS, created a format description, created a
VideoToolbox decompression session and displayed decoded frames.

## Session lifecycle

Date: 24 March 2026

Verified behavior:

- registration with `VID0`
- acknowledgement with `VACK`
- one-second heartbeats
- pause on application backgrounding
- decoder teardown while backgrounded
- re-registration on foregrounding
- recovery on the next natural IDR
- transition to IDLE after disconnect or heartbeat timeout

## On-device inference

Date: 24 March 2026

Verified behavior:

- six models loaded
- inference task ran concurrently with video capture
- detection metadata transmitted on UDP port 3335
- iOS overlay rendered a received detection
- corrected packed-YUV converter produced working face, person and hand results

These records describe the tested hardware, not a universal compatibility claim.
