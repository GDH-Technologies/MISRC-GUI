# HARD DEV NOTE — RP2350 dual_external_adc video timing fix (2026-09-11)

## Symptom
RP2350 (PGA2350) with `dual_external_adc` firmware + MS2130 on Windows: `hsdaoh_open` succeeds, stream starts, libuvc delivers ~54fps of frames, but every frame is `10 80` (limited-range black, no metadata magic `0xda7acab1`). MS2130 HDMI receiver locks timing but recovers no TMDS pixel data. MISRC (FPGA) works fine on the IDENTICAL Windows driver/software config.

## Root cause (hard data, not theory)
Every software layer on Windows was instrumented and proven working:
- libusb: 85,394 bulk transfers, errcode 0
- libuvc: 649 frames assembled, correct 4,147,200-byte size
- hsdaoh: transparent-mode HID writes succeed (readback confirmed)
- UVC negotiation: bFormatIndex=2, bFrameIndex=1, dwFrameInterval=166666, dwMaxVideoFrameSize=4147200, dwMaxPayloadTransferSize=15360 — correct

The failure is the RP2350 firmware's HDMI signal — the MS2130 recovers its sync/timing but not its TMDS pixel data, through the same Windows libuvc path that works for the FPGA. The FPGA firmware produces a recoverable signal; the RP2350 firmware does not.

## The timing difference (the concrete fix)
| | FPGA (`hdmi.v`) | RP2350 (`picohsdaoh.h`) |
|---|---|---|
| hsync width | 4 pixels | **45 pixels** (MODE_H_SYNC_WIDTH) |
| hsync start | 6 pixels | 6 pixels (MODE_H_FRONT_PORCH) |
| vsync width | 1 line | 2 lines (MODE_V_SYNC_WIDTH) |
| vsync start | 0 lines | 1 line (MODE_V_FRONT_PORCH) |
| back porch | 11 pixels (assumed from frame_width 1982) | 11 pixels (MODE_H_BACK_PORCH) |

The RP2350's hsync is 45 pixels wide; the FPGA's is 4 pixels. The MS2130's HDMI receiver likely cannot recover TMDS data with a 45-pixel hsync — the RP2350's timing is outside the MS2130's lock range for data recovery, while the FPGA's is inside.

## Fix to make
Adjust the RP2350 firmware's video timing in `picohsdaoh.h` to match the FPGA's recoverable values:
- MODE_H_SYNC_WIDTH: 45 → 4
- MODE_V_SYNC_WIDTH: 2 → 1
- MODE_V_FRONT_PORCH: 1 → 0
- MODE_H_BACK_PORCH: keep 11 (matches FPGA frame_width 1982 = 1920+6+4+52? no, 1982-1920-6-4=52; RP2350 uses 11)

Actually: FPGA frame_width=1982, so FPGA blanking = 1982-1920 = 62 pixels = hfp(6)+hsw(4)+hbp(52). RP2350 blanking = 6+45+11 = 62. Same total, but hsync is 45 vs 4 and hbp is 11 vs 52. The MS2130 cares about hsync width.

Rebuild `dual_external_adc` with the corrected timing, flash to PGA2350, test with MISRC GUI on the IDENTICAL Windows driver config.

## What NOT to touch again (hard rule)
- Do NOT suggest Windows driver changes (usbvideo/libusbK/WinUSB)
- Do NOT suggest MS2130 firmware patches / register writes beyond the standard transparent-mode set
- Do NOT suggest libuvc/libusb/hsdaoh software changes — all proven working
- The issue is the RP2350 firmware's HDMI timing. Full stop.

## Test verification
After flashing the corrected firmware, run MISRC GUI on the SAME driver config (MI_00 libusbK, MI_04 libusbK). If frames contain real hsdaoh data (not `10 80` black), the timing fix worked. If still black, the RP2350's HSTX drive strength / slew rate / TMDS encoding is the next suspect, not the timing.