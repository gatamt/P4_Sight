# Plan: Lock In The Working `P4_Ai_NY` IMX708 Baseline

## Verified State

- `P4_Ai_NY/GATA_P4_ESP_VIDEO` is now a working bring-up app for:
  - FireBeetle 2 ESP32-P4 v1.0
  - Raspberry Pi Camera Module 3 / IMX708
- Verified on hardware by clean build, flash, and live monitor
- Actual frames are being received, not just sensor detection

## What Was Required To Make It Work

- Standardize on the dedicated `ESP-IDF v5.4.1` release tree
- Vendor the `GuilhermeRS11/esp-video-components` baseline
- Fix MIPI sensor detect config so MIPI sensors are probed with a fully initialized config
- Fix ISP startup so `240 MHz` uses `ISP_CLK_SRC_PLL240`
- Fix ISP-driver error unwind so failed starts do not poison later retries
- Follow the working project’s bring-up style:
  - LDO3 hard power-cycle
  - pre-emptive IMX708 reset
  - explicit LP-11 `stream-off`
  - conservative `1280x720` scale-only mode
  - `esp_video` pipeline instead of the old raw CSI app
  - runtime `csi_buf_afull_thrd = 2040`

## Proven Runtime Evidence

- IMX708 detected correctly with chip ID `0x0708`
- No bogus XCLK configuration during power-on
- No ISP clock-source mismatch failure
- No `no available isp processor`
- Bridge activity appears after `STREAMON`
- First frame received: `1382400 bytes`
- Multiple follow-up frames received
- Smoke test completes successfully

## Practical Working Baseline

- Keep `GATA_P4_ESP_VIDEO` as the golden baseline
- Keep the old `GATA_P4` path out of the critical path unless specific back-porting is needed
- Keep diagnostics safe and minimal enough that they do not crash the app

## Next Work

1. Save and inspect captured frames to validate image quality and pixel correctness.
2. Reduce debug logging and keep only the diagnostics needed for regression testing.
3. Decide whether to port the proven path back into the old app or retire that code path.
4. If needed, add persistent frame dump or image-storage support on top of the now-working baseline.
