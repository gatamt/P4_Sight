# Troubleshooting

## No sensor detected

- Verify the Camera Module 3 cable orientation and connector pitch.
- Verify GPIO 7/8 are the intended camera-control pins for the board.
- Confirm the sensor address is `0x1a` and the ID registers return `0x0708`.
- Confirm LDO channel 3 reaches 2.5 V and is not already owned by another driver.

## Sensor detected, no CSI activity

- Confirm the selected mode uses two lanes and a 450 MHz link clock.
- Confirm the register table ends in a valid stream state.
- Confirm stream-off and reset occur before framework detection.
- Avoid the old raw-bypass capture path on this hardware baseline.

## CSI activity, no dequeued frame

- Check ISP clock-source configuration.
- Check that ISP and CSI resources were released after earlier failed starts.
- Use the heavy reset path instead of repeatedly calling stream-on.
- Verify all capture buffers were queued before stream-on.

## Correct byte count, incorrect image

- Treat the frame as packed `O_UYY_E_VYY`, not I420/NV12/NV21.
- Even rows carry U samples; odd rows carry V samples.
- Use full-range BT.601 conversion.
- Verify sensor orientation and ISP Bayer order are consistent.

## UDP registration fails

- Confirm the phone is connected to `GATA_P4_VIDEO`.
- Confirm it received an address in `192.168.4.0/24`.
- Confirm iOS local-network permission is enabled.
- Confirm the firmware log level is capped at INFO.
- Confirm port 3334 is not used by another socket.

## VideoToolbox errors after resume

Invalidate the old decompression session when the application moves to the
background. On resume, re-register with `VID0` and wait for the next IDR containing
SPS/PPS before creating a fresh session.

## Inference colors or detections are wrong

- Verify the packed converter's U/V row assignment.
- Verify RGB output order is `[R, G, B]`.
- Do not apply an additional R/B swap globally; model preprocessors handle their
  own expected channel order.
- Confirm the RGB buffers are in PSRAM and large enough for the selected input.
