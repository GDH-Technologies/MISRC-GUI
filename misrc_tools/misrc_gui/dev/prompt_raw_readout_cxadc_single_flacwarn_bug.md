# PROMPT — RAW readout fix + CXADC single-card mode + FLAC-off startup warning

Session started 2026-09-28. Tracking all input/output/commands per project rule
(NMQjtpUMr3H6IMUClKEuUE: prompt readme on host with all output/input/commands).

## User request (input)
Three feature/bug-fix changes:
1. RAW data output value when FLAC is off is wrong for readout.
2. Allow for single channel mode if only 1 CX Card is detected.
3. Do not warn re FLAC threads=0 on startup if FLAC is turned off by user.

Pre-commands run by user: `cd '..'` then `cd 'MISRC-GUI'` (pwd = /home/harry/MISRC-GUI).

## Root-cause analysis (verified from code, not assumed)

### Issue 1 — RAW readout wrong when FLAC off
- Per-channel stats panel (`render_channel_stats`, gui_ui.c ~6136-6167) prints
  `buf_rec_raw` from `values->raw_bytes` = `atomic_load(&app->recording_raw_a/b)`
  (passed via `stats[]` at gui_ui.c ~6477-6483). When FLAC is off, only the
  `RAW:` row is shown (FLAC/ratio rows are skipped because
  `comp_bytes == 0 && !use_flac`).
- `raw_writer_thread` (gui_record.c) incremented `recording_raw_a/b` (and
  `recording_bytes`) by `in_len` = `GUI_RECORD_WRITER_BLOCK_BYTES` = the INPUT
  int16 block bytes. Comment literally said "Approximate byte accounting: count
  input bytes consumed". The actual on-disk output is `write_bytes = out_n * bps`
  where `bps` = 1 (8-bit) or 2 (16-bit), and `out_n` changes when soxr resampling
  is enabled.
- => For 8-bit RAW the readout showed ~2x the real file size; for resampled RAW
  it was off by the resample ratio; for 16-bit RAW no-resample it happened to
  match. Same wrong value fed the status-bar runway
  (`gui_ui_recording_output_total_bytes` returns `raw_total` when `!use_flac`,
  gui_ui.c ~1789) and the disk-space guard
  (`gui_record_get_effective_output_total_bytes` returns `raw_total` when
  `!use_flac`, gui_record.c ~734). So runway/disk-guard were also pessimistic
  for 8-bit RAW.
- FLAC-on path unchanged: `flac_writer_thread` still counts `recording_raw_a/b`
  by `raw_bytes_per_block` (input source bytes) for the meaningful
  raw/compressed ratio.

### Issue 2 — single channel mode for 1 CX card
- CXADC device entries (gui_capture.c ~1470-1486): `dev->index` is the CARD
  COUNT (1 = single stock card "[CXADC] CXADC"; 2 = 2-card "[CXADC] CXADC
  Clockgen"). Confirmed by `gui_ui_map_cxadc_channel_to_card` (gui_ui.c ~429:
  `int card_count = dev->index;`).
- Capture start (gui_capture.c ~1894-1933) already supports single-card:
  `cxadc_cards = dev->index` clamped [1,2]; `capture_has_channel_b = (cxadc_cards > 1)`;
  `gui_capture_apply_cxadc_profile(app, 1)` forces `capture_b = false` when
  `card_count < 2`. `gui_cxadc_start(app, 1, false)` is fully supported — the
  capture thread (gui_cxadc.c ~1735) only reads card B when `ctx->card_count > 1`.
- `gui_ui_sync_capture_mode_state` (gui_ui.c ~1449-1497) already forces
  `capture_b = false` for single-card CXADC and sets `single_channel_device`.
- The settings panel already grays out the Capture B toggle for single-card
  CXADC with status "Single-card CXADC has no RF channel B source"
  (gui_ui.c ~8965-9017).
- GAP: `render_channels_panel` (gui_ui.c ~6451-6473) set
  `single_channel_preview = ddd_single_channel || fx3_single_channel` — it did
  NOT include single-card CXADC. So the main view still rendered a dead/empty
  Channel B row (VU B + oscilloscope B + stats B) even though capture_b is
  forced off and there is no channel-B data. DdD/FX3 hide that row; CXADC did not.

### Issue 3 — FLAC threads=0 startup warning when FLAC off
- `misrc_gui.c` main loop (~777-806) fires one-time startup popups for
  `flac_threads == 0` (auto) and `flac_level in 1..3` (low) regardless of
  `app.settings.use_flac`. With FLAC turned off by the user, no FLAC encoder
  runs, so both warnings are noise. The click-handler warnings
  (FlacThreadsMinus / FlacLevelMinus in gui_ui.c ~9039-9077) are deliberate
  interactive actions and are left as-is; only the startup popups are gated.

## Fixes applied (code)

### gui_record.c — `raw_writer_thread` byte accounting
- Normal path (~line 1527): increment `recording_bytes` and
  `recording_raw_a/b` by `write_bytes` (actual output bytes written) instead of
  `in_len` (input int16 block bytes). On the success path `write_ok` is true,
  i.e. `written == write_bytes`, so this equals the real bytes written to the
  .raw file.
- Retry/recovery path (~line 1425, inside the write-error retry block):
  increment by `written` (the bytes actually written on the successful retry)
  instead of `in_len`.
- Net effect: with FLAC off, `recording_raw_a/b` == real on-disk .raw file
  size, so the per-channel `RAW:` readout, the status-bar runway, and the
  disk-space guard all reflect the true output. FLAC-on path untouched.

### gui_ui.c — `render_channels_panel` single-channel preview
- (~line 6461-6473) Added `cxadc_single_card` =
  `gui_ui_selected_device_is_cxadc(app, &clockgen) && !clockgen`
  (clockgen = `dev->index > 1`, so single card = `index == 1`). Added it to
  `single_channel_preview`. Now single-card CXADC hides the Channel B row and
  lets Channel A fill the panel, matching DdD/FX3. 2-card CXADC clockgen and
  non-CXADC devices are unaffected.

### misrc_gui.c — startup FLAC warning gating
- (~line 778-806) Gated the `flac_threads == 0` startup popup on
  `app.settings.use_flac`. Also gated the symmetric `flac_level in 1..3`
  startup popup on `use_flac` (same reasoning: meaningless with no encoder).
  Click-handler warnings unchanged.

## Commands run (this session)
- `ls -la /home/harry/MISRC-GUI` (+ subdirs) — project structure.
- `grep` for flac_threads / raw output / cxadc card detect — locate code.
- `read_files` on misrc_gui.c, gui_settings.c, gui_record.c, gui_ui.c,
  gui_capture.c, gui_cxadc.c, device_enum.c, gui_app.h — hard-data inspection.
- `meson compile -C build-local` -> linked misrc_gui (only pre-existing
  threading.h cast/unused warnings; no errors).
- `build-local/misrc_gui --smoke-test` -> SMOKE_EXIT=0.
- `meson test -C build-local` -> 10/10 OK
  (gui_waveform_overlay, gui_fft_label, gui_toolbar_layout,
  gui_viewport_layout, gui_device_label, gui_ddd_fifo_status,
  gui_ddd_async_policy, ddd_protocol, gui_ddd_async_fault, gui_stats_layout).
- `python3 misrc_tools/test/ci_guard_tests.py --static-only` -> 30/30 PASS,
  CIGUARD_EXIT=0.

## Files changed
- `misrc_tools/misrc_gui/output/gui_record.c` (raw_writer_thread accounting,
  normal + retry paths).
- `misrc_tools/misrc_gui/ui/gui_ui.c` (render_channels_panel
  single_channel_preview includes single-card CXADC).
- `misrc_tools/misrc_gui/core/misrc_gui.c` (startup FLAC warnings gated on
  use_flac).

## NOT yet validated (pending real-world confirmation on hardware)
These changes affect user-facing GUI elements (readout value, Channel B row
visibility, startup popup). Per project rules, real-world confirmation is
required before declaring them working:
- Issue 1: capture RAW with FLAC OFF in both 8-bit and 16-bit (and a resampled
  RAW run) — confirm the per-channel `RAW:` readout now matches the actual
  .raw file size on disk (`stat`/`ls -l`), and the status-bar Runway estimate
  is sane. Compare a 40 MSPS 8-bit RAW run: readout should be ~half of the old
  (wrong) value.
- Issue 2: with only 1 CX card present, select "[CXADC] CXADC" — confirm the
  Channel B row is hidden and Channel A fills the panel; confirm Capture B
  toggle in Settings is still disabled with the "no RF channel B source"
  message; confirm capture + RAW record of channel A still works. With 2 cards
  ("[CXADC] CXADC Clockgen") confirm the Channel B row is still shown.
- Issue 3: set `use_flac=false` + `flac_threads=0` in saved settings, launch —
  confirm NO "FLAC threads set to auto (0)" popup and NO "FLAC level is low"
  popup appear on startup. Set `use_flac=true` + `flac_threads=0` — confirm
  the threads=0 popup still appears. Also confirm the FlacThreadsMinus click
  warning still fires when interactively stepping to 0.

## Restore point
Per project rule (udQirjAOEYGyA029HzncJp): a restore-point zip + log note will
be created on the host once the user confirms the fixes work on real hardware.

## 2026-09-28 UPDATE — readout accuracy (stat-driven + decimal units)

### User follow-up (hard data)
- Two 10s RAW test captures (12:18:27 and 12:19.13, build
dev-2026-09-28-1c14c0e-dirty) on [CXADC] CXADC Clockgen, A=8-bit resampled
40->20 MSPS, B=16-bit HW. Final log + on-disk stat were 1:1:
  - ChA video_rf_8-bit_20msps: 199,753,610 bytes (log rawA == disk stat).
  - ChB hifi_rf_16-bit: 799,014,912 bytes (log rawB == disk stat).
- But the LIVE GUI readout during capture was reported inaccurate to the
  actual file output ("close but not the 1:1 it should be irrespective of
  modes used"); other users reported it also going OVER the file.

### Root cause of the live discrepancy (two independent bugs)
1. Unit mismatch: `render_channel_stats` formatted `recording_raw_a/b` with
   `%.2f GB` / `%.1f MB` using 1024^3 / 1024^2 (MiB/GiB) but labelled it
   "MB"/"GB". `ls`/file-manager show decimal MB (10^6). So a 199,753,610-byte
   file showed as 190.50 "MB" (MiB) vs the file-manager's 199.8 MB — a
   constant ~4.9% under-report that is mode-independent ("irrespective of
   modes"). This was the "under" report.
2. Writer-atomic leads the flushed file: `raw_writer_thread` bumps
   `recording_raw_a/b` after `fwrite` copies into the FILE* buffer, before
   the OS flushes those bytes to disk. So the atomic can momentarily EXCEED
   the on-disk size by the FILE* buffer lead — the "over" report other users
   saw. The stat()-based readout reads the real flushed file size, so it is
   always <= the atomic and never over the file.

### Fix applied (gui_record.c + gui_record.h + gui_ui.c)
- `gui_record.c`: new `gui_record_get_live_output_bytes(out_a, out_b)` stat()s
  `s_record_path_a/b` and returns the real on-disk file sizes. Declared in
  `gui_record.h`.
- `gui_ui.c` `render_channels_panel`: before building stats, refresh
  `s_live_file_size[2]` via `gui_record_get_live_output_bytes` at 5/s
  (0.2s throttle). In RAW mode, override `stats[].raw_bytes` with the
  on-disk file size; in FLAC mode, override `stats[].compressed_bytes` with
  the on-disk .flac size (the "RAW:" input atomic stays for the ratio).
  Falls back to the writer atomic when stat fails or recording is stopped.
- `gui_ui.c` `render_channel_stats`: switched the readout divisors from
  1024^2/1024^3 to 10^6/10^9 (decimal SI MB/GB) so the number matches
  `ls`/file-manager and is 1:1 with the on-disk byte count. Changed
  `%.1f MB` -> `%.2f MB` for consistency. Also updated
  `gui_ui_stats_layout`'s `record_size_max` budget string to
  `UINT64_MAX / 1e9` to match.
- `test/gui_stats_layout_harness.c`: updated the two hardcoded
  `"RAW: 17179869184.00 GB"` (UINT64_MAX/1024^3) assertions to
  `"RAW: 18446744073.71 GB"` (UINT64_MAX/1e9) to match the new decimal
  budget. Tests pass 10/10.

### Net effect
- The per-channel RAW/FLAC readout now shows the exact on-disk file size
  (1:1 with `ls -l`/file-manager), refreshing 5/s, in decimal MB/GB.
- "Under" (MiB vs decimal) and "over" (FILE* buffer lead) are both fixed.
- The end-of-recording log still uses the writer atomic (which matches disk
  to the byte once flushed at stop) — unchanged.

## 2026-09-28 UPDATE — RAW output filenames use .u8 / .u16 extensions

### User request
"also fix raw output modes to use the correct .u8 and .u16 extensions"

### Fix applied (gui_record.c + gui_settings.c)
- Both files had their own static `rf_bits_for_raw` (8/16 clamp) and 8
  `.raw` format strings each (4 per channel) in the auto-name generators:
  `gui_record_apply_auto_names` (gui_record.c, used at record start) and
  `gui_settings_refresh_auto_names` (gui_settings.c, keeps derived names in
  sync for UI display).
- Added `static const char *raw_ext_for_bits(uint8_t bits)` to both files:
  returns `"u8"` for 8-bit, `"u16"` for 16-bit. Matches the
  ld-decode/cxadc raw-sample convention so downstream tools pick the sample
  width from the extension.
- Switched all 8 `.raw` format strings in each file to `.%s` with the
  per-channel extension. The extension is per-channel because bits can
  differ per channel (the test capture had A=8-bit, B=16-bit).
- Example: `Peater_Pan_..._video_rf_8-bit_20msps.raw` ->
  `Peater_Pan_..._video_rf_8-bit_20msps.u8`; `_hifi_rf_16-bit.raw` ->
  `_hifi_rf_16-bit.u16`.
- FLAC-mode filenames (.flac) and audio (.wav) unchanged. The legacy
  `raw_filename` default ("raw_data.bin") and the non-auto-name
  `output_filename_a/b` defaults ("rfA_capture.flac"/"rfB_capture.flac")
  are unchanged (user-set fallbacks when auto naming is off).
- Verified no remaining `.raw` extension references in GUI/test code (grep).

## Commands run (additional, this session)
- `stat -c '%s bytes'` on the two RAW files of each test run — exact byte
  counts to compare against log + GUI readout.
- `meson compile -C build-local` -> BUILD_EXIT=0 (only pre-existing warnings).
- `build-local/misrc_gui --smoke-test` -> SMOKE_EXIT=0.
- `meson test -C build-local` -> 10/10 OK (including updated
  gui_stats_layout fractional-glyph test).
- `python3 misrc_tools/test/ci_guard_tests.py --static-only` -> 30/30 PASS.
- `grep -rn '\.raw"' misrc_tools/misrc_gui misrc_tools/test` -> no remaining
  `.raw` extension references after the switch.

## NOT yet validated (pending real-world confirmation on hardware)
- Readout accuracy: during a RAW capture, confirm the live ChA/ChB "RAW:"
  readout now matches `ls -l`/file-manager decimal MB of the growing .raw
  file (no longer ~5% under, and no longer over the file mid-capture).
- Extensions: confirm a new RAW capture writes `.u8` for 8-bit and `.u16`
  for 16-bit (per channel), and the capture log FILE_PATH_A/B lines show the
  new extensions. (Existing old `.raw` files are not renamed.)
- Restore-point zip will be created once the user confirms all of:
  (1) readout 1:1 with file, (2) .u8/.u16 extensions, (3) single-card
  CXADC hides Channel B, (4) no FLAC threads=0 popup when FLAC off.
