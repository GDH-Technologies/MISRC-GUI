# MISRC GUI — Technical Notes

This file consolidates the code-focused technical notes that previously lived as
standalone files (`GUI_README.md`, `MISRC_GUI_README.md`) in the upstream
`harrypm/MISRC` repo. The user-facing project README is `README.md`; this
document is for developers working on the GUI/tool source.

The packages contain two command-line applications, `misrc_capture` and
`misrc_extract`, plus the `misrc_gui` graphical application. For detailed usage
information see `misrc_tools/README.md`.

## Source build dependency note (FFT)

`misrc_gui` requires FFT support via FFTW single-precision (`fftw3f`). Source
builds fail at configure time if FFTW is missing.

Install FFTW development packages before running Meson:

- Debian/Ubuntu/Linux Mint: `libfftw3-dev`
- macOS (Homebrew): `fftw`
- MSYS2 MinGW x86_64: `mingw-w64-x86_64-fftw`
- MSYS2 MinGW arm64: `mingw-w64-clang-aarch64-fftw`

If you already configured a Meson build directory before installing FFTW, wipe
and reconfigure so stale dependency paths are removed:

    meson setup --wipe misrc_tools/build misrc_tools

## Local AppImage test build (Ubuntu 22.04 baseline)

For a reproducible local AppImage build that targets a `glibc 2.35` baseline,
run:

    ./scripts/build-appimage-local.sh

The script uses `docker` or `podman` (prefers docker if both are installed),
installs build dependencies in an `ubuntu:22.04` container, and writes the
output AppImage to `.ci-artifacts/linux-appimage/`.

If you want to run directly on your host (with all dependencies already
installed), use:

    ./scripts/build-appimage-local.sh --native

## GUI Readout + Stats Breakdown

This section documents what the GUI stats/readouts show, where each value comes
from, and what the wait/drop counters actually mean.

### Scope

- Focus is the live GUI readout/counter behavior in `misrc_gui`.
- Definitions below are based on current code paths in:
  - `misrc_tools/misrc_gui/ui/gui_ui.c`
  - `misrc_tools/misrc_gui/input/gui_capture.c`
  - `misrc_tools/misrc_gui/processing/gui_extract.c`
  - `misrc_tools/misrc_gui/output/gui_record.c`
  - `misrc_tools/common/buffer_manager.c`

### Data flow (where counters are produced)

- Capture callback writes raw RF into `BUF_CAPTURE_RF`.
- Extraction thread reads `BUF_CAPTURE_RF`, updates sample/clip/peak stats, writes:
  - display frames to `BUF_DISPLAY`
  - recording data to `BUF_RECORD_A` / `BUF_RECORD_B` (only while recording)
- Writer threads drain `BUF_RECORD_A/B` and update recording byte counters.

### Bottom status bar readouts

Rendered in `render_status_bar()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `REC dot + HH:MM:SS`
  - Shown only while recording.
  - Time is `GetTime() - recording_start_time`.
- Status text (when not recording)
  - Shows `app->status_message`.
- `Sync: OK` / `Sync: --`
  - Uses `stream_synced`.
- `XX MSPS`
  - From `sample_rate` displayed as integer `sample_rate / 1000000`.
- `Samples`
  - Uses `samples_a` (channel A sample counter).
  - Formatted as raw count with `K/M/G` suffixes.
- `Frames`
  - Uses `frame_count`.
- `Missed`
  - Uses `missed_frame_count`.
  - This counter is debounced in GUI capture callback logic: isolated single miss events are suppressed, and only persistent/consecutive miss conditions increment it.
- `Errors`
  - Uses total `error_count`.
  - This counter is debounced in GUI capture callback logic and tracks persistent parser-error events rather than summing every per-line parser error value.
- `RF Buffer`
  - Percent fill computed from `BUF_CAPTURE_RF` ringbuffer head/tail.
- `Audio Buffer`
  - Percent fill computed from `BUF_CAPTURE_AUDIO` ringbuffer head/tail.

### Side channel stats panels (CH A / CH B)

Rendered by `render_channel_stats()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `Peak: +X% -Y%`
  - Based on `vu_*.peak_pos/peak_neg` (VU peak-hold values, not instantaneous raw ADC values).
  - Source peaks are derived from extraction stats and then smoothed/held in `gui_app_update_vu_meters()`.
- `Clip: +N -M`
  - Cumulative clip counts from extraction thread:
    - positive clip when sample `>= 2047`
    - negative clip when sample `<= -2048`
- `RST` button
  - Clears that channel's clip counters only.
- `Errors`
  - Displays `error_count_a` / `error_count_b`.
  - Current code resets these counters at capture start but does not increment them in active processing paths, so they remain `0` unless future wiring is added.
- During recording only:
  - `RAW: X MB`
    - From `recording_raw_a` / `recording_raw_b`.
  - `FLAC: Y MB` (FLAC mode only)
    - From `recording_compressed_a` / `recording_compressed_b`.
  - `Ratio: Zx` (FLAC mode only)
    - `raw_bytes / compressed_bytes`.

### Record counter placement

- Recording duration counter is in the bottom bar (left side), next to the red record indicator.
- Side channel panels carry per-channel recording size stats (`RAW/FLAC/Ratio`), not the global timer.

### Wait/drop counters: exact meaning

Wait/drop are backpressure metrics tied to ringbuffer write behavior.

#### Buffer-manager definition

In `bufmgr_write_begin()` (`misrc_tools/common/buffer_manager.c`):
- `write_waits` increments when producer must wait for space.
- `write_drops` increments when write is dropped (immediate-drop policy or retries exhausted).

So:
- **wait** = write had to pause because buffer was full.
- **drop** = write could not be queued and was discarded.

#### Policies by path

Default policies in `misrc_tools/common/buffer_manager.c`:
- `BUF_CAPTURE_RF`: wait up to 10 attempts × 5 ms, then drop.
- `BUF_CAPTURE_AUDIO`: immediate drop (no waiting).
- `BUF_RECORD_A/B`: default 200 × 5 ms, but GUI extract record path overrides this.
- `BUF_DISPLAY`: short wait, then drop (display is intentionally lossy).

GUI capture callback overrides in `misrc_tools/misrc_gui/input/gui_capture.c` (aligned to CLI timing):
- RF callback writes use `8 attempts × 1 ms`.
- Audio callback writes use `8 attempts × 1 ms`.

Record-path override in `misrc_tools/misrc_gui/processing/gui_extract.c`:
- `s_record_write_policy = immediate-drop (0 wait attempts, 0 ms timeout)`.
- This means recording writes never block extraction; when record buffers are full, record frames are dropped.

#### Counters used by GUI app state

`gui_app_t` has:
- `rb_wait_count`
- `rb_drop_count`

Current behavior:
- `rb_wait_count` and `rb_drop_count` are updated from `BUF_CAPTURE_RF` buffer-manager write stats deltas each callback.
- In upstream mode, `rb_drop_count` can also be incremented by parsed hsdaoh overrun messages.

#### Where wait/drop is visible today

- Capture stop log (`gui_app_stop_capture()`):
  - prints `waits` and `drops` from app-level counters.
- Recording stop logs (`gui_record_stop()`):
  - prints recording-session wait/drop totals computed from `BUF_RECORD_A/B` deltas.
  - also prints per-buffer `A` and `B` wait/drop deltas.
- Periodic debug log from buffer manager:
  - one-line per-buffer fill/wait/drop summary.

### Rawness and interpretation notes

- Most counters are monotonic event counts since capture start.
- `Missed` is an event count ("missed at least one frame" events), not an exact per-frame-loss total.
- `Missed` and `Errors` are intentionally debounced in GUI capture mode to avoid one-off transient spikes from dominating the UI readout.
- GUI capture also applies a callback-gap resync guard: if callback timing stalls for >100 ms (for example due to system/display interruptions), parser sync state is reset before continuing so stale parser state does not generate a long burst of follow-on errors.
- `Peak` in side panels is VU peak-hold representation, not raw unsmoothed instantaneous sample.
- Buffer percentages are instantaneous snapshot values.

## Capture / regression development notes

In-tree capture-path constraints and historical regression notes (macOS
scheduling, parser CRC, tolerated-frame behavior, etc.) live in:

    misrc_tools/misrc_gui/dev/dev_notes_README.md

Key constraints to preserve when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when
  `result.error_count > 0 && result.report_errors`. Do not reject tolerated
  CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null
  checks), before width/height early returns. This prevents false
  timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start,
  explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, the audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO ->
  gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after
  capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`,
  and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.
